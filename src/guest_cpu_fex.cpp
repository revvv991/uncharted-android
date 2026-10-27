// g2-arm64: guest_cpu.h on ARM64 hosts — guest code runs on an embedded FEXCore.
// Modeled on Bachata S4's src/core/fex/fex_guest_engine.cpp and runtime/probes/fexcore-smoke.cpp
// (MIT, github.com/zenithblue-oss/shadps4-arm64), adapted to the uncharted_pc host.
//
// Guest -> host: every host function gets a 16-byte x86 veneer
//     mov r10, rcx ; mov eax, <id> ; syscall ; ret
// FEX leaves the JIT on the syscall (HandleSyscall below), the host function runs with the SysV
// arguments moved to AAPCS64 registers (x0..x7 <- rdi rsi rdx rcx r8 r9 [rsp+8] [rsp+16],
// d0..d7 <- xmm0..7), and its x0/x1/d0/d1 become rax/rdx/xmm0/xmm1. The veneer's ret returns.
// Host -> guest: ExecuteThread up to a hlt (outermost call on a host thread) or HandleCallback
// (a nested call from inside a host function).
#include "guest_cpu.h"
extern "C" {
void runtime_report(void);
int runtime_fiber_describe(char* out, size_t size, uint64_t rsp);
}

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>

#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using FEXCore::X86State::REG_RAX;
using FEXCore::X86State::REG_RCX;
using FEXCore::X86State::REG_RDX;
using FEXCore::X86State::REG_RSI;
using FEXCore::X86State::REG_RDI;
using FEXCore::X86State::REG_RSP;
using FEXCore::X86State::REG_RBP;
using FEXCore::X86State::REG_R8;
using FEXCore::X86State::REG_R9;
using FEXCore::X86State::REG_R10;

[[noreturn]] void Fatal(const char* what) {
  std::fprintf(stderr, "STOP: guest CPU (FEX): %s\n", what);
  std::fflush(nullptr);
  _exit(25);
}

// ---- host functions and their veneers ------------------------------------------------------
constexpr size_t kVeneerSize = 16;
constexpr size_t kVeneerRetOffset = 10; // the ret after `syscall`
constexpr size_t kChunk = 64 * 1024;

struct HostFunction {
  const void* fn;
  const char* name;
  GuestAbi abi;
  uintptr_t veneer;
};

std::mutex g_functions_mutex;
std::vector<HostFunction> g_functions; // id = index
std::atomic<uintptr_t> g_veneer_begin {0}, g_veneer_end {0};
uint8_t* g_chunk = nullptr;
size_t g_chunk_used = kChunk;

// One mapping for every veneer page so a range check finds them (MAP_NORESERVE: 64 MiB of address
// space, pages are touched as veneers are written).
constexpr size_t kVeneerSpace = 64ull << 20;
uint8_t* g_veneer_space = nullptr;
size_t g_veneer_space_used = 0;

uintptr_t AllocateVeneer(uint32_t id) {
  if (g_chunk_used + kVeneerSize > kChunk) {
    if (g_veneer_space_used + kChunk > kVeneerSpace) Fatal("out of veneer space");
    g_chunk = g_veneer_space + g_veneer_space_used;
    g_veneer_space_used += kChunk;
    g_chunk_used = 0;
    if (mprotect(g_chunk, kChunk, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) Fatal("veneer mprotect");
  }
  uint8_t* code = g_chunk + g_chunk_used;
  g_chunk_used += kVeneerSize;
  const uint8_t bytes[kVeneerSize] = {0x49, 0x89, 0xca,                          // mov r10, rcx
                                      0xb8, uint8_t(id), uint8_t(id >> 8),       // mov eax, id
                                      uint8_t(id >> 16), uint8_t(id >> 24),
                                      0x0f, 0x05,                                // syscall
                                      0xc3,                                      // ret
                                      0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
  std::memcpy(code, bytes, sizeof(bytes));
  __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + kVeneerSize));
  return reinterpret_cast<uintptr_t>(code);
}

// ---- AAPCS64 call with SysV-derived arguments ----------------------------------------------
// bb_call_host(fn, x[8], d[8], out[4], stack[8]): x0..x7 = x[], d0..d7 = d[], out = {x0, x1,
// d0, d1}; stack[] = the integer arguments past the eighth (AAPCS64 passes them on the stack;
// AJM batch functions take ten).
extern "C" void bb_call_host(const void* fn, const uint64_t* x, const uint64_t* d, uint64_t* out,
                             const uint64_t* stack);
asm(R"(
  .text
  .globl bb_call_host
  .type bb_call_host, %function
bb_call_host:
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  str x19, [sp, #16]
  sub sp, sp, #64
  ldp x9, x10, [x4]
  stp x9, x10, [sp]
  ldp x9, x10, [x4, #16]
  stp x9, x10, [sp, #16]
  ldp x9, x10, [x4, #32]
  stp x9, x10, [sp, #32]
  ldp x9, x10, [x4, #48]
  stp x9, x10, [sp, #48]
  mov x16, x0
  mov x19, x3
  ldp d0, d1, [x2]
  ldp d2, d3, [x2, #16]
  ldp d4, d5, [x2, #32]
  ldp d6, d7, [x2, #48]
  mov x17, x1
  ldp x0, x1, [x17]
  ldp x2, x3, [x17, #16]
  ldp x4, x5, [x17, #32]
  ldp x6, x7, [x17, #48]
  blr x16
  stp x0, x1, [x19]
  stp d0, d1, [x19, #16]
  add sp, sp, #64
  ldr x19, [sp, #16]
  ldp x29, x30, [sp], #32
  ret
  .size bb_call_host, .-bb_call_host
)");

// ---- guest threads ---------------------------------------------------------------------------
class GuestSegments {
public:
  void Initialize(FEXCore::Core::CPUState& state) {
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt.data();
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt.data();
    state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* cs = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(cs, 0);
    FEXCore::Core::CPUState::SetGDTLimit(cs, 0xF'FFFFU);
    state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*cs);
    cs->L = 1;
    cs->D = 0;
  }

private:
  std::array<FEXCore::Core::CPUState::gdt_segment, 32> gdt {};
};

struct HostThread {
  FEXCore::Core::InternalThreadState* fex = nullptr;
  GuestSegments* segments = nullptr;
  uint64_t gs_base = 0;
  uint8_t* stack_top = nullptr;
  int depth = 0;                                // guest code running below this host frame
  FEXCore::Core::CpuStateFrame* hle_frame = nullptr; // inside a host function the guest called
  uint32_t hle_id = 0;
  bool hle_replaced = false;                    // guest_context_set during that call
  uint32_t tid = 0;                             // gettid(), for the host function trace
};
thread_local HostThread t_host;

// g2-arm64: the host function trace (diagnosis), HleTraceStart below: each record one call.
struct HleRecord {
  uint32_t id, tid;
  uint64_t caller;             // the guest's call site
  uint64_t args[3];            // the first three integer arguments
  uint64_t enter_ns, leave_ns; // CLOCK_MONOTONIC
};
constexpr size_t kHleMaxRecords = 1 << 20;
HleRecord* g_hle = nullptr;
std::atomic<size_t> g_hle_n {0};
std::atomic<bool> g_hle_on {false};

uint64_t MonotonicNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

FEXCore::Context::Context* g_context = nullptr;
FEXCore::HostFeatures g_features {};
uintptr_t g_exit_hlt = 0;        // hlt: the outermost guest_call returns here
uintptr_t g_callback_return = 0; // FEX CALLBACKRET: nested calls return here

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
  Syscalls() { OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC; }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments*) override {
    auto& st = frame->State;
    const uint32_t id = static_cast<uint32_t>(st.gregs[REG_RAX]);
    HostFunction f;
    {
      std::scoped_lock lock {g_functions_mutex};
      if (id >= g_functions.size()) Fatal("syscall outside a veneer (guest executed a raw syscall)");
      f = g_functions[id];
    }
    uint64_t x[8], d[8], out[4], stack[8];
    const uint64_t rsp = st.gregs[REG_RSP]; // [rsp] = return address of the veneer call
    // Integer arguments 9..16 are read too (the guest caller's frame is above rsp).
    if (f.abi == GUEST_ABI_MS) {
      x[0] = st.gregs[REG_R10]; x[1] = st.gregs[REG_RDX]; x[2] = st.gregs[REG_R8]; x[3] = st.gregs[REG_R9];
      std::memcpy(&x[4], reinterpret_cast<const void*>(rsp + 8 + 32), 4 * sizeof(uint64_t));
      std::memcpy(stack, reinterpret_cast<const void*>(rsp + 8 + 32 + 32), sizeof(stack));
    } else {
      x[0] = st.gregs[REG_RDI]; x[1] = st.gregs[REG_RSI]; x[2] = st.gregs[REG_RDX]; x[3] = st.gregs[REG_R10];
      x[4] = st.gregs[REG_R8]; x[5] = st.gregs[REG_R9];
      std::memcpy(&x[6], reinterpret_cast<const void*>(rsp + 8), 2 * sizeof(uint64_t));
      std::memcpy(stack, reinterpret_cast<const void*>(rsp + 8 + 16), sizeof(stack));
    }
    for (int i = 0; i < 8; ++i) d[i] = st.xmm.sse.data[i][0];

    HostThread& t = t_host;
    auto* const saved_frame = t.hle_frame;
    const uint32_t saved_id = t.hle_id;
    const bool saved_replaced = t.hle_replaced;
    t.hle_frame = frame; t.hle_id = id; t.hle_replaced = false;
    const bool trace = g_hle_on.load(std::memory_order_relaxed);
    uint64_t caller = 0, enter = 0;
    if (trace) {
      std::memcpy(&caller, reinterpret_cast<const void*>(rsp), sizeof(caller));
      enter = MonotonicNs();
    }
    bb_call_host(f.fn, x, d, out, stack);
    if (trace) {
      const size_t i = g_hle_n.fetch_add(1, std::memory_order_relaxed);
      if (i < kHleMaxRecords) g_hle[i] = {id, t.tid, caller, {x[0], x[1], x[2]}, enter, MonotonicNs()};
    }
    const bool replaced = t.hle_replaced;
    t.hle_frame = saved_frame; t.hle_id = saved_id; t.hle_replaced = saved_replaced;
    if (replaced) return st.gregs[REG_RAX]; // guest_context_set wrote the whole state

    st.gregs[REG_RDX] = out[1];
    st.xmm.sse.data[0][0] = out[2];
    st.xmm.sse.data[1][0] = out[3];
    st.gregs[REG_RAX] = out[0];
    return out[0];
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    // The whole address space: the image, veneers, hooks. Self-modifying code is not expected.
    return {0, std::numeric_limits<uint64_t>::max(), true};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*,
                                                                                uint64_t) override {
    return std::nullopt;
  }
};

class Signals final : public FEXCore::SignalDelegator {
public:
  uintptr_t GetThunkCallbackRET() const override { return g_callback_return; }
};

Syscalls* g_syscalls = nullptr;
Signals* g_signals = nullptr;

constexpr size_t kTempStack = 1 << 20;

void ProfRegisterThread();

void EnsureThread() {
  HostThread& t = t_host;
  if (t.fex) return;
  if (!t.stack_top) {
    // A host thread the guest did not create (decoder callbacks, the main thread before
    // guest_call_on_stack): a private guest stack.
    void* stack = mmap(nullptr, kTempStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) Fatal("cannot map a guest stack");
    t.stack_top = static_cast<uint8_t*>(stack) + kTempStack;
  }
  t.fex = g_context->CreateThread(0, reinterpret_cast<uint64_t>(t.stack_top) - 16);
  if (!t.fex) Fatal("CreateThread failed");
  void* callret = mmap(nullptr, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (callret == MAP_FAILED) Fatal("cannot map the call-return stack");
  t.fex->CallRetStackBase = callret;
  auto& state = t.fex->CurrentFrame->State;
  state.callret_sp = reinterpret_cast<uint64_t>(callret) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
  t.segments = new GuestSegments;
  t.segments->Initialize(state);
  // The eboot reads its TCB through gs (fs:[0] loads rewritten by the linker scripts); other
  // modules may still use fs: both point at the guest TCB.
  state.gs_cached = state.fs_cached = t.gs_base;
  state.mxcsr = 0x9fc0; // PS4 default: flush to zero, denormals are zero (runtime_set_guest_fpu)
  state.FCW = 0x037f;
  ProfRegisterThread();
}

// Guest sampling profiler (diagnosis). BB_GUEST_PROF=<seconds> with BB_GUEST_PROF_TRIGGER=<file>:
// once the file appears (it is deleted), every guest thread gets SIGPROF each millisecond for
// that long; the handler maps the host pc in translated code back to the guest rip
// (RestoreRIPFromHostPC). The log gets the hottest guest rips with their code bytes, the host
// functions the guest was inside, and samples per thread.
// g2-arm64: a host function's samples carry the guest's call site (and the frame above it through
// rbp), and each thread gets its own top entries: where each game thread waits, called from where.
struct ProfSample {
  uint64_t key; // guest rip, or host function id
  uint64_t caller, caller2; // a host function's: the guest's return address, the one above it
  uint32_t tid;
  uint8_t kind; // 0 translated code, 1 host function, 2 elsewhere (dispatcher, JIT compiler)
};
constexpr size_t kProfMaxThreads = 256;
constexpr size_t kProfMaxSamples = 1 << 21;
std::atomic<uint32_t> g_prof_tids[kProfMaxThreads];
std::atomic<uint32_t> g_prof_ntids {0};
ProfSample* g_prof = nullptr;
std::atomic<size_t> g_prof_n {0};
std::atomic<bool> g_prof_on {false};

// Guest memory without a fault on a bad address (a signal handler reading a guessed frame).
bool ProfRead(void* out, uint64_t address, size_t size) {
  iovec local {out, size}, remote {reinterpret_cast<void*>(address), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

void ProfSignal(int, siginfo_t*, void* ctx) {
  if (!g_prof_on.load(std::memory_order_relaxed)) return;
  const int saved_errno = errno;
  HostThread& t = t_host;
  if (t.fex) {
    const auto pc = static_cast<uint64_t>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
    ProfSample sample {0, 0, 0, static_cast<uint32_t>(gettid()), 2};
    if (g_context->IsAddressInCodeBuffer(t.fex, pc)) {
      sample.kind = 0;
      sample.key = g_context->RestoreRIPFromHostPC(t.fex, pc);
    } else if (t.hle_frame) {
      sample.kind = 1;
      sample.key = t.hle_id;
      // [rsp] at the veneer's syscall is the call site; rbp, when it looks like the caller's
      // frame pointer, gives the frame above it.
      const auto& st = t.hle_frame->State;
      const uint64_t rsp = st.gregs[REG_RSP], rbp = st.gregs[REG_RBP];
      uint64_t frame[2];
      ProfRead(&sample.caller, rsp, 8);
      if (rbp > rsp && rbp - rsp < (64u << 10) && !(rbp & 7) && ProfRead(frame, rbp, sizeof(frame))) {
        sample.caller2 = frame[1];
      }
    } else {
      sample.key = t.fex->CurrentFrame->State.rip;
    }
    const size_t i = g_prof_n.fetch_add(1, std::memory_order_relaxed);
    if (i < kProfMaxSamples) g_prof[i] = sample;
  }
  errno = saved_errno;
}

void ProfRegisterThread() {
  t_host.tid = static_cast<uint32_t>(gettid());
  const uint32_t i = g_prof_ntids.fetch_add(1);
  if (i < kProfMaxThreads) g_prof_tids[i].store(t_host.tid);
}

struct ProfEntry {
  uint8_t kind;
  uint64_t key, caller, caller2;
  size_t count;
};

bool ProfSame(const ProfSample& a, const ProfSample& b) {
  return a.kind == b.kind && a.key == b.key && a.caller == b.caller && a.caller2 == b.caller2;
}

// Entries of sorted samples (equal ones adjacent), the most frequent first.
std::vector<ProfEntry> ProfGroup(const ProfSample* begin, const ProfSample* end, size_t* per_kind) {
  std::vector<ProfEntry> entries;
  for (const ProfSample* i = begin; i < end;) {
    const ProfSample* j = i;
    while (j < end && ProfSame(*j, *i)) ++j;
    entries.push_back({i->kind, i->key, i->caller, i->caller2, static_cast<size_t>(j - i)});
    if (per_kind) per_kind[i->kind] += j - i;
    i = j;
  }
  std::sort(entries.begin(), entries.end(), [](const ProfEntry& a, const ProfEntry& b) { return a.count > b.count; });
  return entries;
}

void ProfPrint(const char* indent, const ProfEntry& e, double percent, bool bytes) {
  if (e.kind == 1) {
    const char* name = e.key < g_functions.size() ? g_functions[e.key].name : nullptr;
    std::printf("%s%5.1f%%  host function %s from %#llx", indent, percent, name ? name : "?",
                (unsigned long long)e.caller);
    if (e.caller2) std::printf(" (above it %#llx)", (unsigned long long)e.caller2);
    std::printf("\n");
    return;
  }
  std::printf("%s%5.1f%%  %s rip %#llx", indent, percent, e.kind == 0 ? "guest" : "other", (unsigned long long)e.key);
  if (bytes) {
    // The code around it, for a disassembler: 32 bytes before, 64 from the rip.
    std::printf("  bytes@-32:");
    const auto* code = reinterpret_cast<const uint8_t*>(e.key - 32);
    for (int k = 0; k < 96; ++k) std::printf("%s%02x", k == 32 ? "|" : "", code[k]);
  }
  std::printf("\n");
}

void ProfReport(size_t n, double secs) {
  std::sort(g_prof, g_prof + n, [](const ProfSample& a, const ProfSample& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.key != b.key) return a.key < b.key;
    return a.caller != b.caller ? a.caller < b.caller : a.caller2 < b.caller2;
  });
  size_t per_kind[3] = {};
  const auto entries = ProfGroup(g_prof, g_prof + n, per_kind);
  std::printf("Guest profile: %zu samples in %.1f s: translated code %.1f%%, host functions %.1f%%, "
              "elsewhere %.1f%%\n",
              n, secs, 100.0 * per_kind[0] / n, 100.0 * per_kind[1] / n, 100.0 * per_kind[2] / n);
  int shown = 0, dumped = 0;
  for (const auto& e : entries) {
    if (shown++ == 60) break;
    const bool bytes = e.kind == 0 && dumped < 16;
    dumped += bytes;
    ProfPrint("  ", e, 100.0 * e.count / n, bytes);
  }
  // Per thread (a stable sort keeps each thread's samples in the order above): its entries of 2%
  // and up.
  std::stable_sort(g_prof, g_prof + n, [](const ProfSample& a, const ProfSample& b) { return a.tid < b.tid; });
  for (size_t i = 0; i < n;) {
    size_t j = i, k0 = 0, k1 = 0;
    while (j < n && g_prof[j].tid == g_prof[i].tid) {
      k0 += g_prof[j].kind == 0;
      k1 += g_prof[j].kind == 1;
      ++j;
    }
    char path[64], name[32] = "?";
    std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", g_prof[i].tid);
    if (FILE* f = std::fopen(path, "r")) {
      if (std::fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
      std::fclose(f);
    }
    std::printf("  thread %u %-16s %6zu samples (%.0f%% of the ticks): translated %zu, host %zu\n",
                g_prof[i].tid, name, j - i, 100.0 * (j - i) / (secs * 1000.0), k0, k1);
    int k = 0;
    for (const auto& e : ProfGroup(g_prof + i, g_prof + j, nullptr)) {
      if (k++ == 8 || e.count * 50 < j - i) break;
      ProfPrint("      ", e, 100.0 * e.count / (j - i), false);
    }
    i = j;
  }
  std::fflush(stdout);
}

void ProfThread(double secs, std::string trigger) {
  for (;;) {
    while (access(trigger.c_str(), F_OK) != 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unlink(trigger.c_str());
    std::printf("Guest profile: sampling for %.0f s\n", secs);
    std::fflush(stdout);
    g_prof_n = 0;
    g_prof_on = true;
    const auto begin = std::chrono::steady_clock::now();
    const auto end = begin + std::chrono::duration<double>(secs);
    const pid_t pid = getpid();
    while (std::chrono::steady_clock::now() < end) {
      const uint32_t count = std::min<uint32_t>(g_prof_ntids.load(), kProfMaxThreads);
      for (uint32_t i = 0; i < count; ++i)
        if (const uint32_t tid = g_prof_tids[i].load()) syscall(SYS_tgkill, pid, tid, SIGPROF);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_prof_on = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // handlers in flight
    const size_t n = std::min(g_prof_n.load(), kProfMaxSamples);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    if (n) ProfReport(n, took);
  }
}

void ProfStart() {
  const char* secs = std::getenv("BB_GUEST_PROF");
  const char* trigger = std::getenv("BB_GUEST_PROF_TRIGGER");
  if (!secs || !trigger) return;
  g_prof = static_cast<ProfSample*>(std::calloc(kProfMaxSamples, sizeof(ProfSample)));
  if (!g_prof) return;
  struct sigaction sa {};
  sa.sa_sigaction = ProfSignal;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);
  std::thread(ProfThread, std::atof(secs), std::string(trigger)).detach();
  std::printf("Guest profile: armed (%s s), trigger file %s\n", secs, trigger);
}

// g2-arm64: the host function trace (diagnosis). BB_HLE_TRACE=<seconds> with
// BB_HLE_TRACE_FILE=<file>: once <file>.trigger appears (it is deleted), every host function call
// of the guest is recorded for that long (at most 1M calls; one that has not returned 200 ms after
// the window is not): the function, the thread, the guest's call site, the first three integer
// arguments, when it was entered and left. Written to <file> (HleRecord's) with the functions'
// names in <file>.names and the threads' in <file>.threads ("<id> <name>" lines); read by
// scripts/g2/hle_trace.py (what blocks which thread how long; the frame pacing).
void HleTraceThread(double secs, std::string file) {
  const std::string trigger = file + ".trigger";
  for (;;) {
    while (access(trigger.c_str(), F_OK) != 0) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unlink(trigger.c_str());
    std::printf("Host function trace: recording for %.1f s\n", secs);
    std::fflush(stdout);
    g_hle_n = 0;
    g_hle_on = true;
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
    g_hle_on = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // calls in flight
    const size_t calls = g_hle_n.load(), n = std::min(calls, kHleMaxRecords);
    bool written = false;
    if (FILE* out = std::fopen(file.c_str(), "wb")) {
      written = std::fwrite(g_hle, sizeof(HleRecord), n, out) == n;
      written = std::fclose(out) == 0 && written;
    }
    if (FILE* names = std::fopen((file + ".names").c_str(), "w")) {
      std::scoped_lock lock {g_functions_mutex};
      for (size_t i = 0; i < g_functions.size(); ++i)
        std::fprintf(names, "%zu %s\n", i, g_functions[i].name ? g_functions[i].name : "?");
      std::fclose(names);
    }
    if (FILE* threads = std::fopen((file + ".threads").c_str(), "w")) {
      const uint32_t count = std::min<uint32_t>(g_prof_ntids.load(), kProfMaxThreads);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t tid = g_prof_tids[i].load();
        char path[64], name[32] = "?";
        std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
        if (FILE* f = std::fopen(path, "r")) {
          if (std::fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
          std::fclose(f);
        }
        std::fprintf(threads, "%u %s\n", tid, name);
      }
      std::fclose(threads);
    }
    std::printf("Host function trace: %zu calls%s, %s %s\n", calls,
                calls > n ? " (the buffer kept the first 1M)" : "", written ? "written to" : "NOT written to",
                file.c_str());
    std::fflush(stdout);
  }
}

void HleTraceStart() {
  const char* secs = std::getenv("BB_HLE_TRACE");
  const char* file = std::getenv("BB_HLE_TRACE_FILE");
  if (!secs || !file) return;
  g_hle = static_cast<HleRecord*>(std::calloc(kHleMaxRecords, sizeof(HleRecord)));
  if (!g_hle) return;
  std::thread(HleTraceThread, std::atof(secs), std::string(file)).detach();
  std::printf("Host function trace: armed (%s s), trigger file %s.trigger\n", secs, file);
}

} // namespace

extern "C" {

int guest_cpu_init(void) {
  if (g_context) return 0;
  if (sysconf(_SC_PAGESIZE) != 4096) {
    std::fprintf(stderr, "guest CPU (FEX): needs 4 KiB pages\n");
    return -1;
  }
  FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
  FEXCore::Config::Initialize();
  // g2-arm64: FEX's options from the environment, FEX_<OPTION>=value (on the G2 from the APK's
  // env.txt), for experiments such as FEX_TSOENABLED=0 or FEX_MULTIBLOCK=0; the settings below
  // override them. Without this layer FEX read no options at all here (only its defaults).
  FEXCore::Config::AddLayer(FEX::Config::CreateEnvironmentLayer(environ));
  for (char** e = environ; e && *e; ++e) {
    if (std::strncmp(*e, "FEX_", 4) == 0) std::fprintf(stderr, "Guest CPU (FEX): option %s\n", *e);
  }
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
  // rdtsc in translated code reads the bare virtual counter, as sceKernelReadTsc does, so that
  // it agrees with sceKernelGetTscFrequency (the counter's measured rate, 19.2 MHz on the G2).
  // FEX scales a slow counter up to >= 1 GHz by default: the guest then took each second of
  // rdtsc time for 64 (game time ran ahead, clamped only by the engine's longest step).
  // BB_TSC_SCALE=1: FEX's scaling.
  {
    const char* scale = std::getenv("BB_TSC_SCALE");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_SMALLTSCSCALE, scale && scale[0] == '1' ? "1" : "0");
  }
  g_features = FEX::FetchHostFeatures();
  g_context = FEXCore::Context::Context::CreateNewContext(g_features).release();
  if (!g_context) return -1;

  g_veneer_space = static_cast<uint8_t*>(
    mmap(nullptr, kVeneerSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (g_veneer_space == MAP_FAILED) return -1;
  g_veneer_begin = reinterpret_cast<uintptr_t>(g_veneer_space);
  g_veneer_end = g_veneer_begin + kVeneerSpace;
  // Return stubs in the first chunk: hlt, then FEX's CALLBACKRET (0f 3e).
  g_chunk = g_veneer_space;
  g_veneer_space_used = kChunk;
  if (mprotect(g_chunk, kChunk, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return -1;
  g_chunk[0] = 0xf4;
  g_chunk[16] = 0x0f; g_chunk[17] = 0x3e;
  __builtin___clear_cache(reinterpret_cast<char*>(g_chunk), reinterpret_cast<char*>(g_chunk + 32));
  g_exit_hlt = reinterpret_cast<uintptr_t>(g_chunk);
  g_callback_return = reinterpret_cast<uintptr_t>(g_chunk + 16);
  g_chunk_used = 32;

  g_syscalls = new Syscalls;
  g_signals = new Signals;
  g_context->SetSignalDelegator(g_signals);
  g_context->SetSyscallHandler(g_syscalls);
  g_context->EnableExitOnHLT();
  if (!g_context->InitCore()) return -1;
  std::printf("Guest CPU: FEXCore (AVX %s)\n", g_features.SupportsAVX ? "yes" : "no");
  ProfStart();
  HleTraceStart();
  return 0;
}

uintptr_t guest_host_function(const void* fn, const char* name, GuestAbi abi) {
  if (!fn) return 0;
  std::scoped_lock lock {g_functions_mutex};
  for (const auto& f : g_functions)
    if (f.fn == fn && f.abi == abi) return f.veneer;
  const uint32_t id = static_cast<uint32_t>(g_functions.size());
  const uintptr_t veneer = AllocateVeneer(id);
  g_functions.push_back({fn, name, abi, veneer});
  return veneer;
}

static void SetArguments(FEXCore::Core::CPUState& state, const uint64_t* args, unsigned count) {
  static constexpr int regs[6] = {REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9};
  for (unsigned i = 0; i < count && i < 6; ++i) state.gregs[regs[i]] = args[i];
}

// A guest trap (int n, INTO, a privileged instruction) is a Break op. With ExitOnHLT on, FEX's
// GuestSignal_SIGSEGV stub returns from the outermost ExecuteThread the way our hlt does (from a
// nested HandleCallback too: it unwinds to the outermost dispatcher), so the guest function looked
// as if it had returned and its thread ended without a word: after one of ND's assertions
// (`int $0x41`) two job workers vanished and the game hung. Report it and stop, as the PC port does.
// The hlt that ends an outermost call sets the same fault data (trap 13) at g_exit_hlt.
static void CheckGuestTrap(HostThread& t) {
  const auto& fault = t.fex->CurrentFrame->SynchronousFaultData;
  const auto& st = t.fex->CurrentFrame->State;
  if (!fault.FaultToTopAndGeneratedException || st.rip == g_exit_hlt) return;
  char line[192], name[17] = {};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  int n = std::snprintf(line, sizeof(line), "Guest fault (trap %u, error %#x", (unsigned)fault.TrapNo,
                        (unsigned)fault.err_code);
  if (fault.TrapNo == FEXCore::X86State::X86_TRAPNO_GP && (fault.err_code & 7) == 2)
    n += std::snprintf(line + n, sizeof(line) - n, ": int %#x", (unsigned)fault.err_code >> 3);
  std::snprintf(line + n, sizeof(line) - n, ") at guest rip %#llx, thread %s\n", (unsigned long long)st.rip, name);
  std::fputs(line, stderr);
  t.hle_frame = nullptr; // the registers are the trap's, also under a host function
  char more[1536];
  int m = guest_cpu_describe_registers(more, sizeof(more));
  if (m > 0) std::fwrite(more, 1, (size_t)m, stderr);
  m = guest_cpu_describe_stack(more, sizeof(more));
  if (m > 0) std::fwrite(more, 1, (size_t)m, stderr);
  m = runtime_fiber_describe(more, sizeof(more), st.gregs[REG_RSP]);
  if (m > 0) std::fwrite(more, 1, (size_t)m, stderr);
  runtime_report();
  std::fflush(nullptr);
  _exit(25);
}

static uint64_t CallOutermost(uintptr_t entry, const uint64_t* args, unsigned count, uint64_t rsp_top) {
  HostThread& t = t_host;
  auto& state = t.fex->CurrentFrame->State;
  uint64_t rsp = (rsp_top & ~uint64_t(15)) - 8 * (count > 6 ? ((count - 6 + 1) & ~1u) : 0);
  for (unsigned i = 6; i < count; ++i) std::memcpy(reinterpret_cast<void*>(rsp + 8 * (i - 6)), &args[i], 8);
  rsp -= 8;
  std::memcpy(reinterpret_cast<void*>(rsp), &g_exit_hlt, 8); // return address: hlt
  SetArguments(state, args, count);
  state.gregs[REG_RSP] = rsp;
  state.rip = entry;
  ++t.depth;
  g_context->ExecuteThread(t.fex);
  --t.depth;
  CheckGuestTrap(t);
  return state.gregs[REG_RAX];
}

uint64_t guest_call(uintptr_t entry, const uint64_t* args, unsigned count) {
  EnsureThread();
  HostThread& t = t_host;
  auto& state = t.fex->CurrentFrame->State;
  // 4 KiB above the guest rsp: natively such calls run on the host stack with the caller's frames
  // above; code that reads past its own frame (an SRT walker read stack_top on G2, the guard page
  // of a heap-allocated stack) must find memory there too.
  if (t.depth == 0) return CallOutermost(entry, args, count, reinterpret_cast<uint64_t>(t.stack_top) - 4096);
  if (count > 6) Fatal("nested guest_call with stack arguments");
  // Nested: a host function the guest called calls guest code. Keep the interrupted state.
  uint64_t saved_gregs[16];
  std::memcpy(saved_gregs, state.gregs, sizeof(saved_gregs));
  const uint64_t saved_rip = state.rip;
  auto saved_xmm = state.xmm;
  SetArguments(state, args, count);
  state.gregs[REG_RSP] = (state.gregs[REG_RSP] - 256) & ~uint64_t(15); // below the caller's frame
  ++t.depth;
  g_context->HandleCallback(t.fex, entry);
  --t.depth;
  const uint64_t result = state.gregs[REG_RAX];
  std::memcpy(state.gregs, saved_gregs, sizeof(saved_gregs));
  state.rip = saved_rip;
  state.xmm = saved_xmm;
  return result;
}

uint64_t guest_call_on_stack(uintptr_t entry, const uint64_t* args, unsigned count, void* stack_top) {
  t_host.stack_top = static_cast<uint8_t*>(stack_top);
  EnsureThread();
  return CallOutermost(entry, args, count, reinterpret_cast<uint64_t>(stack_top));
}

void guest_set_gs_base(uint64_t base) {
  t_host.gs_base = base;
  if (t_host.fex) {
    auto& state = t_host.fex->CurrentFrame->State;
    state.gs_cached = state.fs_cached = base;
  }
}

void guest_set_stack(void* stack_top) { t_host.stack_top = static_cast<uint8_t*>(stack_top); }

int guest_cpu_signal(int sig, void* info_, void* context_) {
  // Unaligned atomics in translated code (FEX emits LSE atomics; a misaligned guest atomic
  // raises SIGBUS with BUS_ADRALN, which FEX fixes up), as Bachata's HandleGuestSignal does.
  auto* info = static_cast<siginfo_t*>(info_);
  auto* context = static_cast<ucontext_t*>(context_);
  HostThread& t = t_host;
  if (sig != SIGBUS || !info || !context || !t.fex || info->si_code != BUS_ADRALN) return 0;
  const auto pc = static_cast<uintptr_t>(context->uc_mcontext.pc);
  if (!g_context->IsAddressInCodeBuffer(t.fex, pc)) return 0;
  auto* registers = reinterpret_cast<uint64_t*>(context->uc_mcontext.regs);
  const auto adjustment = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
    t.fex, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc, registers);
  if (!adjustment) return 0;
  context->uc_mcontext.pc = pc + *adjustment;
  // g2-arm64: FEX emulates an access that crosses 16 bytes with two aligned ones and backpatches
  // the spot. For TSO-ordered plain loads/stores (LDAPR/STLR) that is fine; a read-modify-write
  // (a guest lock-prefixed op) split that way is not strictly atomic (no split-lock mutex by
  // default): log where those happen, count the rest.
  const uint32_t instr = *reinterpret_cast<const uint32_t*>(pc);
  const bool rmw = (instr & 0x3F200C00u) == 0x38200000u    // LDADD/LDCLR/LDEOR/LDSET/.../SWP
                   || (instr & 0x3FA07C00u) == 0x08A07C00u // CAS
                   || (instr & 0xBFA07C00u) == 0x08207C00u; // CASP
  static std::atomic<uint64_t> unaligned_rmw, unaligned_plain;
  const uint64_t n = (rmw ? unaligned_rmw : unaligned_plain).fetch_add(1, std::memory_order_relaxed) + 1;
  if (rmw ? (n <= 16 || (n & (n - 1)) == 0) : (n & (n - 1)) == 0 && n >= 64) {
    char line[200];
    const int len = rmw ? std::snprintf(line, sizeof(line), "Guest CPU: split atomic RMW #%llu at guest rip %#llx (address %p, insn %08x)\n",
                                        (unsigned long long)n, (unsigned long long)g_context->RestoreRIPFromHostPC(t.fex, pc),
                                        info->si_addr, instr)
                        : std::snprintf(line, sizeof(line), "Guest CPU: %llu unaligned ordered loads/stores backpatched so far\n",
                                        (unsigned long long)n);
    if (len > 0) { ssize_t w_ = write(2, line, (size_t)len); (void)w_; }
  }
  return 1;
}

int guest_context_get(GuestContext* out) {
  HostThread& t = t_host;
  if (!t.hle_frame) return 0;
  const auto& st = t.hle_frame->State;
  std::memcpy(out->gpr, st.gregs, sizeof(out->gpr));
  out->rip = g_functions[t.hle_id].veneer + kVeneerRetOffset; // resumes at the veneer's ret
  out->mxcsr = st.mxcsr;
  out->fcw = st.FCW;
  return 1;
}

int guest_context_set(const GuestContext* in) {
  HostThread& t = t_host;
  if (!t.hle_frame) return 0;
  auto& st = t.hle_frame->State;
  std::memcpy(st.gregs, in->gpr, sizeof(in->gpr));
  // The JIT continues with the veneer's ret after the syscall: a state that resumes elsewhere
  // gets its rip pushed, so that ret jumps there with rsp as requested.
  uint64_t veneer_ret = 0;
  {
    std::scoped_lock lock {g_functions_mutex};
    const uint64_t r = in->rip;
    if (r >= g_veneer_begin && r < g_veneer_end && (r & (kVeneerSize - 1)) == kVeneerRetOffset) veneer_ret = r;
  }
  if (!veneer_ret) {
    st.gregs[REG_RSP] -= 8;
    std::memcpy(reinterpret_cast<void*>(st.gregs[REG_RSP]), &in->rip, 8);
  }
  st.mxcsr = in->mxcsr;
  st.FCW = in->fcw;
  t.hle_replaced = true;
  return 1;
}

int guest_cpu_describe_thread(char* out, size_t size) {
  HostThread& t = t_host;
  if (!t.fex) return 0;
  const auto& st = t.fex->CurrentFrame->State;
  if (!t.hle_frame) {
    return std::snprintf(out, size, "  guest rip=%#llx rsp=%#llx (in guest code or idle)\n",
                         (unsigned long long)st.rip, (unsigned long long)st.gregs[REG_RSP]);
  }
  uint64_t ret = 0;
  std::memcpy(&ret, reinterpret_cast<const void*>(st.gregs[REG_RSP]), sizeof(ret));
  const char* name = t.hle_id < g_functions.size() ? g_functions[t.hle_id].name : "?";
  return std::snprintf(out, size, "  guest in host function %s, called from %#llx (rdi=%#llx rsi=%#llx)\n",
                       name ? name : "?", (unsigned long long)ret, (unsigned long long)st.gregs[REG_RDI],
                       (unsigned long long)st.gregs[REG_RSI]);
}

int guest_cpu_describe_registers(char* out, size_t size) {
  HostThread& t = t_host;
  if (!t.fex || size < 512) return 0;
  const auto& st = t.hle_frame ? t.hle_frame->State : t.fex->CurrentFrame->State;
  static const char* const names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  size_t n = (size_t)std::snprintf(out, size, "  guest registers (%s):\n",
                                   t.hle_frame ? "at the host function call" : "last sync in guest code");
  for (int r = 0; r < 16 && n + 100 < size; r += 4) {
    n += (size_t)std::snprintf(out + n, size - n, "  ");
    for (int k = r; k < r + 4; ++k)
      n += (size_t)std::snprintf(out + n, size - n, " %-3s=%016llx", names[k], (unsigned long long)st.gregs[k]);
    n += (size_t)std::snprintf(out + n, size - n, "\n");
  }
  // 256 bytes at rbx (the object a method works on, in the usual SysV code), read with
  // process_vm_readv: an unmapped address prints nothing instead of faulting again.
  uint64_t words[32];
  struct iovec local = {words, sizeof(words)};
  struct iovec remote = {reinterpret_cast<void*>(st.gregs[FEXCore::X86State::REG_RBX]), sizeof(words)};
  if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)sizeof(words)) {
    for (int i = 0; i < 32 && n + 100 < size; i += 4)
      n += (size_t)std::snprintf(out + n, size - n, "  [rbx+%#04x] %016llx %016llx %016llx %016llx\n", i * 8,
                                 (unsigned long long)words[i], (unsigned long long)words[i + 1],
                                 (unsigned long long)words[i + 2], (unsigned long long)words[i + 3]);
  }
  return (int)n;
}

uint64_t guest_cpu_guest_rsp(void) {
  HostThread& t = t_host;
  if (!t.fex) return 0;
  const auto& st = t.hle_frame ? t.hle_frame->State : t.fex->CurrentFrame->State;
  return st.gregs[REG_RSP];
}

int guest_cpu_describe_stack(char* out, size_t size) {
  HostThread& t = t_host;
  if (!t.fex || size < 64) return 0;
  const auto& st = t.fex->CurrentFrame->State;
  // 4 KiB above rsp, read with process_vm_readv: an unmapped page ends the read instead of faulting.
  uint64_t words[512];
  struct iovec local = {words, sizeof(words)};
  struct iovec remote = {reinterpret_cast<void*>(st.gregs[REG_RSP]), sizeof(words)};
  const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  if (got <= 0) return 0;
  size_t n = (size_t)std::snprintf(out, size, "  guest stack:");
  for (size_t i = 0, shown = 0; i < (size_t)got / 8 && shown < 32 && n + 24 < size; i++) {
    if (words[i] < 0x400000 || words[i] >= 0x80000000ull) continue;
    n += (size_t)std::snprintf(out + n, size - n, " %#llx", (unsigned long long)words[i]);
    shown++;
  }
  n += (size_t)std::snprintf(out + n, size - n, "\n");
  return (int)n;
}

} // extern "C"
