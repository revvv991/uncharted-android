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
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/HLE/SourcecodeResolver.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/fextl/memory.h>

#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sched.h>
#include <ucontext.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <pthread.h>
#include <ctime>
#include <limits>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <cctype>
#include <set>
#include <span>
#include <utility>

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
  uint64_t result;             // the integer result
};
constexpr size_t kHleMaxRecords = 1 << 20;
HleRecord* g_hle = nullptr;
std::atomic<size_t> g_hle_n {0};
std::atomic<bool> g_hle_on {false};
std::atomic<bool> g_hle_ring {false}; // BB_HLE_TRACE_RING: the last kHleMaxRecords calls are kept
std::string g_hle_file;

uint64_t MonotonicNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// g2-arm64 diagnosis: BB_HLE_SLOW_MS=<ms> - host function calls that took at least that long are
// printed with the time, the thread, the call site and three arguments (sleeps, and U1's idle wait
// on its job manager's condition variable 0x13a9ce0, left out), and gaps of over 70 ms between the
// guest's flip submissions on the same clock (U1's game-side stalls, findings 2026-11-01 evening).
double HleSlowMs() {
  static const double ms = [] {
    const char* e = std::getenv("BB_HLE_SLOW_MS");
    return e ? std::atof(e) : 0.0;
  }();
  return ms;
}
bool HleSleepName(const char* n) {
  return n && (!std::strcmp(n, "sceKernelUsleep") || !std::strcmp(n, "usleep") || !std::strcmp(n, "sleep") ||
               !std::strcmp(n, "nanosleep") || !std::strcmp(n, "sceKernelNanosleep") || !std::strcmp(n, "sceKernelSleep"));
}
bool HleFlipName(const char* n) {
  return n && (!std::strncmp(n, "sceGnmSubmitAndFlipCommandBuffers", 33) || !std::strcmp(n, "sceVideoOutSubmitFlip"));
}
std::atomic<uint64_t> g_hle_last_flip_ns {0};

FEXCore::Context::Context* g_context = nullptr;
FEXCore::HostFeatures g_features {};
uintptr_t g_exit_hlt = 0;        // hlt: the outermost guest_call returns here
uintptr_t g_callback_return = 0; // FEX CALLBACKRET: nested calls return here

// ---- g2-arm64: FEX's code cache for the game image (on by default, BB_FEX_CODE_CACHE=0: off) -----
// FEX compiles a guest block the first time it runs. On the G2 (BB_JIT_STATS, findings 2026-11-12)
// U3's process compiles ~12,500 blocks of its image, 1.4 s of compile time over its threads: 0.66 s
// in its first 5 s (0.13 s on the main thread), 0.46 s in the Continue load's heaviest 5 s (0.38 s
// on one job worker); the collection menu's process ~5,900 blocks, 0.55 s. FEXCore can keep
// compiled code across runs (EnableCodeCachingWIP), keyed by an executable file; here the one
// "file" is the game image probe.c builds, named after its path and identified by a hash of its
// executable segments after every patch (FileId), the run-time values in them left out (probe.c's
// note_varying; FEX gets them as Skip relocations and caches no block that holds one); the cache
// file's name also carries the FEX options from the environment, the CPU's features and this
// library's build id (FEX's version string does not cover our FEX patches). The blocks compiled
// while playing are recorded (<image>.<pid>.seen, every 10 s); a second FEXCore context compiles
// the recorded blocks for a cache file on an idle-priority thread (BB_FEX_CACHE_GEN_DELAY: seconds
// after the image load, "30,180" by default: the boot's blocks, then a level's - the later passes
// only for 256 new blocks or more, fewer wait for the next start), and the next run
// maps the file and finalizes the pages of cached code on their first execution (a fault on the
// cache's code buffer, guest_cpu_signal: the page and its neighbours, BB_FEX_CACHE_CHUNK pages at
// most, 16). A warm U3 run compiles no block of its image (10 ms in all: veneers, thunks).
// On by default since port 0185, so a bad cache must not cost a player the game (FEX's SaveData
// checks none of its writes and LoadCache trusts the file):
//  - the file is whole or absent: FEX writes it into memory (a memfd), it goes to disk with every
//    write checked, a trailer (CacheTrailer) and an fsync before the rename; a file without the
//    trailer of this image and build is removed, not mapped;
//  - a crash while a cache is mapped is recorded (<image>.guard, guest_cpu_code_cache_crashed):
//    the next run drops the file, runs on the JIT and compiles a new one; the same crash again
//    within three runs of a mapped cache turns the cache off for this image until another build,
//    and so does a crash on the thread that compiles the cache (it would come back 30 s into
//    every run);
//  - no cache is compiled when the image's code is not what the key says any more (code written
//    after the load), when its hash is not the last run's under the same build (a value that
//    changes from run to run would make every run write 50 MB for nothing: such a run only
//    records), when a thunk was placed by search (its address is not the next run's), or when the
//    disk or the memory is short;
//  - it needs mremap's MREMAP_DONTUNMAP (Linux 5.7) and 4 KiB pages: off with a note otherwise.
// BB_FEX_CACHE_LOAD=0 / BB_FEX_CACHE_GEN=0 leave out the load or the compile (A/Bs on one fixed
// cache); BB_FEX_CACHE_CLEAR=1 drops this image's files (its guard record too). Files:
// $HOME/.cache/fex/<image>.<pid>.seen, <image>.blocks (the folded records), <image>.guard,
// <image>-<FileId>-<config>.fexcache. The safety net's tests: BB_FEX_CACHE_TEST (CacheTest below).

// The last bytes of a cache file, after FEX's data: a file that has them is whole.
struct CacheTrailer {
  char magic[8];      // kTrailerMagic
  uint64_t payload;   // FEX's data before the trailer, bytes
  uint64_t file_id;   // the image's code
  uint64_t config_id; // the options, CPU and build it was compiled for
};
constexpr char kTrailerMagic[8] = {'B', 'B', 'F', 'X', 'E', 'N', 'D', '1'};

// <image>.guard: what the last runs of this image did with its cache.
struct CacheGuard {
  char magic[8];      // kGuardMagic
  uint64_t file_id;   // the last run's image code
  uint64_t config_id; // and its options, CPU and build
  uint32_t state;     // kGuardClean, kGuardSuspect, kGuardOff
  uint32_t runs;      // runs that mapped a cache since the crash below
  uint64_t crash;     // the last crash with a cache mapped or being compiled: the signal and the
                      // guest rip (or the host library offset), hashed; 0: none
};
constexpr char kGuardMagic[8] = {'B', 'B', 'F', 'X', 'G', 'R', 'D', '1'};
enum : uint32_t {
  kGuardClean = 0,
  kGuardSuspect = 1, // a run crashed with the cache mapped: the next run drops the file
  kGuardOff = 2,     // the same crash again, or a crash of the compile: no cache for this image
                     // and build
};

struct ImageCode {
  bool on = false, load = false, generate = false;
  bool record = false;                             // this run's blocks go to its .seen file
  uintptr_t base = 0, end = 0;                     // the image, [base, end)
  std::vector<std::pair<uint64_t, uint64_t>> code; // its executable segments: offset, size
  std::vector<uint64_t> slots;                     // the run-time values in them: offsets, sorted
  uint64_t config_id = 0;
  FEXCore::ExecutableFileInfo info;                // FileId: the hash of the image's code
  std::string dir, name, map_path, blocks_path, cache_path;
  fextl::unique_ptr<FEXCore::MappedCodeCacheFile> mapped;
  std::mutex finalize;
  CacheGuard guard {};                             // <image>.guard as this run wrote it
  int guard_fd = -1;
  std::atomic<bool> armed {false};                 // a cache is mapped: a crash is recorded
  std::atomic<bool> crash_noted {false};           // once
};
// Never destroyed: guest threads may still run cached code while the process exits.
ImageCode& g_image = *new ImageCode;

bool EnvOn(const char* name) {
  const char* e = std::getenv(name);
  return e && e[0] == '1';
}
bool EnvOff(const char* name) {
  const char* e = std::getenv(name);
  return e && e[0] == '0';
}

uint64_t HashMix(uint64_t h, uint64_t v) {
  h ^= v * 0x9E3779B97F4A7C15ull;
  h = (h << 27 | h >> 37) * 0xC2B2AE3D27D4EB4Full;
  return h ^ (h >> 31);
}
// 64-bit hash, four lanes (~10 ms for U3's 22 MiB of code)
uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t lane[4] = {seed, seed + 1, seed + 2, seed + 3};
  size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    for (int k = 0; k < 4; ++k) {
      uint64_t v;
      std::memcpy(&v, p + i + 8 * k, 8);
      lane[k] = HashMix(lane[k], v);
    }
  }
  uint64_t h = HashMix(size, lane[0]);
  for (int k = 1; k < 4; ++k) h = HashMix(h, lane[k]);
  for (; i < size; ++i) h = HashMix(h, p[i]);
  return h;
}

// The JIT's compile time (BB_JIT_STATS=1: a line every 5 s). FEX calls PreCompile when it starts
// a block and, with a code map writer set, LookupExecutableFileSection once the block is in (the
// writer records it there): the time between the two, per thread.
bool g_jit_stats = false;
thread_local uint64_t t_compile_start = 0;
thread_local bool t_cache_generator = false; // the code cache's compile thread: not counted
constexpr size_t kJitThreads = 64;
struct JitThread {
  std::atomic<uint64_t> ns {0};
  char name[16] {};
};
JitThread g_jit_threads[kJitThreads];
std::atomic<size_t> g_jit_nthreads {0};
thread_local size_t t_jit_slot = ~size_t(0);
std::atomic<uint64_t> g_jit_blocks {0}, g_jit_outside {0}, g_jit_ns {0}, g_jit_max_ns {0}, g_jit_over2 {0}, g_jit_over10 {0};
std::atomic<uint64_t> g_cache_pages {0}; // code cache pages finalized (their first use)
std::atomic<uint64_t> g_cache_page_ns {0}; // and the time the faulting threads spent on that
std::atomic<uint64_t> g_cache_page_faults {0}; // in that many faults
size_t g_cache_chunk = 16;                 // pages finalized at a fault, at most (BB_FEX_CACHE_CHUNK)

void NoteCompile(uint64_t ns, bool in_image) {
  g_jit_blocks.fetch_add(1, std::memory_order_relaxed);
  if (!in_image) g_jit_outside.fetch_add(1, std::memory_order_relaxed);
  g_jit_ns.fetch_add(ns, std::memory_order_relaxed);
  uint64_t longest = g_jit_max_ns.load(std::memory_order_relaxed);
  while (ns > longest && !g_jit_max_ns.compare_exchange_weak(longest, ns, std::memory_order_relaxed)) {
  }
  if (ns >= 2000000) g_jit_over2.fetch_add(1, std::memory_order_relaxed);
  if (ns >= 10000000) g_jit_over10.fetch_add(1, std::memory_order_relaxed);
  if (t_jit_slot == ~size_t(0)) {
    t_jit_slot = g_jit_nthreads.fetch_add(1, std::memory_order_relaxed);
    if (t_jit_slot < kJitThreads) pthread_getname_np(pthread_self(), g_jit_threads[t_jit_slot].name, sizeof(g_jit_threads[0].name));
  }
  if (t_jit_slot < kJitThreads) g_jit_threads[t_jit_slot].ns.fetch_add(ns, std::memory_order_relaxed);
}

// FEX's code map writer stays closed: it is set only so that FEX looks up each compiled block's
// file section at the end of the compile, where the blocks are recorded below. Its own file took
// the blocks 4 KiB at a time, and a killed run lost the tail: ~265 blocks of a warm U3 run (its
// level start's) were compiled again in every run.
class CodeMapFile final : public FEXCore::CodeMapOpener {
public:
  int OpenCodeMapFile() override {
    return -1;
  }
};
CodeMapFile g_code_map_file;

// The blocks compiled in this run (offsets from the image base): appended to <image>.<pid>.seen
// every 10 s ("BBFXSEEN", the image's FileId, then uint32 offsets), so a killed run loses at most
// its last 10 s.
std::mutex g_seen_mutex;
std::vector<uint32_t> g_seen_new;
std::mutex g_seen_file_mutex;

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
    const double slow_ms = HleSlowMs();
    const uint64_t slow_enter = slow_ms > 0.0 ? MonotonicNs() : 0;
    bb_call_host(f.fn, x, d, out, stack);
    if (slow_ms > 0.0) {
      static const uint64_t t0 = slow_enter;
      const uint64_t leave = MonotonicNs();
      const double ms = (leave - slow_enter) / 1e6;
      if (HleFlipName(f.name)) {
        const uint64_t prev = g_hle_last_flip_ns.exchange(leave, std::memory_order_relaxed);
        if (prev && leave - prev > 70000000)
          std::printf("Guest flip gap: %.3f s, %.0f ms since the last flip\n", (leave - t0) / 1e9, (leave - prev) / 1e6);
      }
      const bool idle_wait = f.name && !std::strcmp(f.name, "scePthreadCondTimedwait") && x[0] == 0x13a9ce0;
      if (ms >= slow_ms && !HleSleepName(f.name) && !idle_wait) {
        char thread[16] = "";
        pthread_getname_np(pthread_self(), thread, sizeof(thread));
        uint64_t site = 0;
        std::memcpy(&site, reinterpret_cast<const void*>(rsp), sizeof(site));
        std::printf("Slow host call: %.3f s %s %s %.1f ms, caller %#" PRIx64 ", args %#" PRIx64 " %#" PRIx64 " %#" PRIx64
                    " -> %#" PRIx64 "\n", (slow_enter - t0) / 1e9, thread, f.name ? f.name : "?", ms, site, x[0], x[1],
                    x[2], out[0]);
      }
    }
    if (trace) {
      const size_t i = g_hle_n.fetch_add(1, std::memory_order_relaxed);
      if (i < kHleMaxRecords || g_hle_ring.load(std::memory_order_relaxed))
        g_hle[i % kHleMaxRecords] = {id, t.tid, caller, {x[0], x[1], x[2]}, enter, MonotonicNs(), out[0]};
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
  // g2-arm64: the game image is FEX's one executable "file" (its code map and code cache,
  // guest_cpu_code_cache); the JIT stats time each compile up to the lookup at its end.
  void PreCompile() override {
    if (g_jit_stats && !t_cache_generator) t_compile_start = MonotonicNs();
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*,
                                                                                uint64_t address) override {
    const bool in_image = address - g_image.base < g_image.end - g_image.base;
    if (t_compile_start) {
      NoteCompile(MonotonicNs() - t_compile_start, in_image);
      t_compile_start = 0;
    }
    if (!in_image) return std::nullopt;
    if (g_image.record && !t_cache_generator) {
      std::lock_guard lock {g_seen_mutex};
      g_seen_new.push_back(static_cast<uint32_t>(address - g_image.base));
    }
    return FEXCore::ExecutableFileSectionInfo {g_image.info, g_image.base, g_image.base, g_image.end};
  }
};

class Signals final : public FEXCore::SignalDelegator {
public:
  uintptr_t GetThunkCallbackRET() const override { return g_callback_return; }
};

Syscalls* g_syscalls = nullptr;
Signals* g_signals = nullptr;

// ---- g2-arm64: the code cache's files, load and compile (see ImageCode above) --------------------
#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif
#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif
// FEX finalizes cache pages with mremap(MREMAP_DONTUNMAP), Linux 5.7 and later.
bool KernelHasDontUnmap() {
  void* from = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void* to = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  bool ok = false;
  if (from != MAP_FAILED && to != MAP_FAILED)
    ok = mremap(from, 4096, 4096, MREMAP_FIXED | MREMAP_MAYMOVE | MREMAP_DONTUNMAP, to) != MAP_FAILED;
  if (from != MAP_FAILED) munmap(from, 4096);
  if (to != MAP_FAILED) munmap(to, 4096);
  return ok;
}

// This library's GNU build id, hashed (0 without one): the FEX it was built with, our patches in.
uint64_t OwnBuildId() {
  struct Find {
    uintptr_t address;
    uint64_t id;
  } find {reinterpret_cast<uintptr_t>(&OwnBuildId), 0};
  dl_iterate_phdr(
    [](dl_phdr_info* info, size_t, void* data) -> int {
      auto* f = static_cast<Find*>(data);
      bool inside = false;
      for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD && f->address - (info->dlpi_addr + ph.p_vaddr) < ph.p_memsz) inside = true;
      }
      if (!inside) return 0;
      for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
        const auto* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
          const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
          const uint8_t* name = p + sizeof(*note);
          const uint8_t* desc = name + ((note->n_namesz + 3) & ~3u);
          if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && !std::memcmp(name, "GNU", 4)) {
            f->id = HashBytes(desc, note->n_descsz, 0x6275696c64ull);
            return 1;
          }
          p = desc + ((note->n_descsz + 3) & ~3u);
        }
      }
      return 1;
    },
    &find);
  return find.id;
}

// The FEX options from the environment (FEX_<OPTION>=..., BB_TSC_SCALE), the CPU's features (the
// JIT's code depends on them: a cache copied to another device with the app's files is not used
// there) and this build; kCacheFormat counts the changes of the cache files' own layout.
constexpr uint64_t kCacheFormat = 2; // 2: the trailer (port 0185)
uint64_t CodeCacheConfigId() {
  std::vector<std::string> options;
  for (char** e = environ; e && *e; ++e)
    if (std::strncmp(*e, "FEX_", 4) == 0) options.emplace_back(*e);
  std::sort(options.begin(), options.end());
  uint64_t id = HashMix(OwnBuildId(), kCacheFormat);
  for (const auto& o : options) id = HashBytes(o.data(), o.size(), id);
  const char* scale = std::getenv("BB_TSC_SCALE");
  const std::string s = scale ? scale : "";
  id = HashBytes(s.data(), s.size(), id);
  const uint64_t cpu[2] = {getauxval(AT_HWCAP), getauxval(AT_HWCAP2)};
  return HashBytes(cpu, sizeof(cpu), id);
}

// $HOME/.cache/fex/ (on Android HOME is the app's files/home)
std::string CodeCacheDir() {
  const char* home = std::getenv("HOME");
  std::string dir = std::string(home && *home ? home : ".") + "/.cache";
  mkdir(dir.c_str(), 0755);
  dir += "/fex";
  mkdir(dir.c_str(), 0755);
  return dir + "/";
}

// ".../u3/boot-linked.bin" -> "u3-boot-linked"
std::string ImageName(const char* path) {
  const std::string p = path && *path ? path : "image";
  const size_t slash = p.find_last_of('/');
  std::string file = slash == std::string::npos ? p : p.substr(slash + 1), parent;
  if (slash != std::string::npos && slash > 0) {
    const size_t before = p.find_last_of('/', slash - 1);
    const size_t from = before == std::string::npos ? 0 : before + 1;
    parent = p.substr(from, slash - from);
  }
  if (const size_t dot = file.find_last_of('.'); dot != std::string::npos && dot > 0) file.resize(dot);
  std::string name = parent.empty() ? file : parent + "-" + file;
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
  return name;
}

const char* Basename(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

bool InImageCode(uint64_t offset) {
  for (const auto& [start, size] : g_image.code)
    if (offset - start < size) return true;
  return false;
}

// The image's files in the cache directory: <prefix>...<suffix>
std::vector<std::string> ImageFiles(const std::string& prefix, const char* suffix) {
  std::vector<std::string> files;
  DIR* d = opendir(g_image.dir.c_str());
  if (!d) return files;
  const size_t n = std::strlen(suffix);
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > prefix.size() + n && name.compare(0, prefix.size(), prefix) == 0 &&
        name.compare(name.size() - n, n, suffix) == 0 && e->d_type != DT_DIR)
      files.push_back(g_image.dir + name);
  }
  closedir(d);
  return files;
}

// A run's block record (<image>.<pid>.seen): blocks of this image's code only (a partial last
// offset of a run killed mid-write is ignored).
void ReadSeen(const std::string& path, std::set<uint32_t>& blocks) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return;
  char magic[8];
  uint64_t id = 0;
  if (std::fread(magic, 1, 8, f) == 8 && !std::memcmp(magic, "BBFXSEEN", 8) && std::fread(&id, 8, 1, f) == 1 &&
      id == g_image.info.FileId) {
    uint32_t chunk[1024];
    size_t n;
    while ((n = std::fread(chunk, 4, 1024, f)) > 0)
      for (size_t i = 0; i < n; ++i)
        if (InImageCode(chunk[i])) blocks.insert(chunk[i]);
  }
  std::fclose(f);
}

// This run's new blocks to its .seen file.
void FlushSeen() {
  std::lock_guard file_lock {g_seen_file_mutex};
  std::vector<uint32_t> fresh;
  {
    std::lock_guard lock {g_seen_mutex};
    fresh.swap(g_seen_new);
  }
  if (fresh.empty()) return;
  struct stat st {};
  const bool new_file = stat(g_image.map_path.c_str(), &st) != 0;
  FILE* f = std::fopen(g_image.map_path.c_str(), "ab");
  if (!f) return;
  if (new_file) {
    std::fwrite("BBFXSEEN", 1, 8, f);
    std::fwrite(&g_image.info.FileId, 8, 1, f);
  }
  std::fwrite(fresh.data(), 4, fresh.size(), f);
  std::fclose(f);
}

// The folded code maps: "BBFXBLK1", the image's FileId, the count, the offsets (uint32, sorted).
std::set<uint32_t> ReadBlockList() {
  std::set<uint32_t> blocks;
  FILE* f = std::fopen(g_image.blocks_path.c_str(), "rb");
  if (!f) return blocks;
  char magic[8];
  uint64_t id = 0, count = 0;
  if (std::fread(magic, 1, 8, f) == 8 && !std::memcmp(magic, "BBFXBLK1", 8) && std::fread(&id, 8, 1, f) == 1 &&
      std::fread(&count, 8, 1, f) == 1 && id == g_image.info.FileId && count < (1u << 24)) {
    std::vector<uint32_t> v(count);
    if (std::fread(v.data(), 4, count, f) == count)
      for (const uint32_t offset : v)
        if (InImageCode(offset)) blocks.insert(offset);
  }
  std::fclose(f);
  return blocks;
}

bool WriteBlockList(const std::set<uint32_t>& blocks) {
  const std::string tmp = g_image.blocks_path + ".new";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const uint64_t count = blocks.size();
  const std::vector<uint32_t> v(blocks.begin(), blocks.end());
  bool ok = std::fwrite("BBFXBLK1", 1, 8, f) == 8 && std::fwrite(&g_image.info.FileId, 8, 1, f) == 1 &&
            std::fwrite(&count, 8, 1, f) == 1 && std::fwrite(v.data(), 4, v.size(), f) == v.size();
  ok = std::fclose(f) == 0 && ok;
  return ok && std::rename(tmp.c_str(), g_image.blocks_path.c_str()) == 0;
}

// The image's code as the cache keys it: the executable segments' bytes, the run-time values in
// them (g_image.slots) left out. ~10 ms for U3's 22 MiB.
uint64_t ImageCodeId() {
  const auto* bytes = reinterpret_cast<const uint8_t*>(g_image.base);
  const uint64_t size = g_image.end - g_image.base;
  uint64_t id = HashBytes(&size, sizeof(size), g_image.base);
  for (const auto& [start, length] : g_image.code) {
    const uint64_t segment[2] = {start, length}, end = start + length;
    id = HashBytes(segment, sizeof(segment), id);
    uint64_t at = start;
    for (auto it = std::lower_bound(g_image.slots.begin(), g_image.slots.end(), start); it != g_image.slots.end() && *it < end; ++it) {
      if (*it < at) continue;
      id = HashBytes(bytes + at, *it - at, id);
      id = HashMix(id, *it ^ 0x76617279ull);
      at = std::min(*it + 8, end);
    }
    id = HashBytes(bytes + at, end - at, id);
  }
  return id;
}

// The guard record to its open file (async-signal-safe: the crash record writes it).
bool WriteGuard() {
  return g_image.guard_fd >= 0 &&
         pwrite(g_image.guard_fd, &g_image.guard, sizeof(g_image.guard), 0) == static_cast<ssize_t>(sizeof(g_image.guard));
}

// The guard record at the start of a run: what the last runs say this one may do.
void OpenGuard() {
  const std::string path = g_image.dir + g_image.name + ".guard";
  CacheGuard last {};
  if (const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0) {
    if (read(fd, &last, sizeof(last)) != static_cast<ssize_t>(sizeof(last))) last = {};
    close(fd);
  }
  const bool known = !std::memcmp(last.magic, kGuardMagic, 8) && last.config_id == g_image.config_id;
  CacheGuard now {};
  std::memcpy(now.magic, kGuardMagic, 8);
  now.file_id = g_image.info.FileId;
  now.config_id = g_image.config_id;
  if (known && last.file_id == g_image.info.FileId) {
    now.state = last.state;
    now.runs = last.runs;
    now.crash = last.crash;
    if (last.state == kGuardSuspect) {
      unlink(g_image.cache_path.c_str());
      now.state = kGuardClean;
      now.runs = 0;
      g_image.load = false;
      std::printf("FEX code cache: the last run crashed with the cache in use (crash %016llx): its file is dropped, this run "
                  "compiles a new one\n", (unsigned long long)last.crash);
    } else if (last.state == kGuardOff) {
      unlink(g_image.cache_path.c_str());
      g_image.load = g_image.generate = g_image.record = false;
      std::printf("FEX code cache: off for this image until another version of the app (the same crash twice with the cache "
                  "in use, or a crash while compiling it: %016llx)\n", (unsigned long long)last.crash);
    }
  } else if (known) {
    g_image.generate = false;
    std::printf("FEX code cache: the image's code is not the last run's (%016llx then): this run only records its blocks, a "
                "cache is compiled once two runs in a row have the same code\n", (unsigned long long)last.file_id);
  }
  g_image.guard = now;
  g_image.guard_fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (!WriteGuard()) {
    g_image.load = g_image.generate = g_image.record = false;
    std::printf("FEX code cache: off in this run (cannot write %s)\n", Basename(path));
  }
}

// Files no run can use: this image's caches of another key (another build, CPU or image code), the
// files a killed run left half-written, FEX's own code maps (the first test builds), its block
// records of another image code; and any image's cache of another build, CPU or options (that
// game may not be started again).
void RemoveStaleFiles() {
  size_t removed = 0;
  const auto drop = [&](const std::string& path) {
    if (unlink(path.c_str()) == 0) ++removed;
  };
  {
    char config[40];
    std::snprintf(config, sizeof(config), "-%016llx.fexcache", (unsigned long long)g_image.config_id);
    const size_t n = std::strlen(config);
    std::vector<std::string> others;
    if (DIR* d = opendir(g_image.dir.c_str())) {
      while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() > n && name.compare(name.size() - 9, 9, ".fexcache") == 0 && name.compare(name.size() - n, n, config) != 0)
          others.push_back(g_image.dir + name);
      }
      closedir(d);
    }
    for (const auto& path : others) drop(path);
  }
  for (const auto& path : ImageFiles(g_image.name + "-", ".fexcache"))
    if (path != g_image.cache_path) drop(path);
  for (const auto& path : ImageFiles(g_image.name + "-", ".fexcache.new")) drop(path);
  for (const auto& path : ImageFiles(g_image.name + ".", ".codemap")) drop(path);
  for (const auto& path : ImageFiles(g_image.name + ".", "blocks.new")) drop(path);
  for (const auto& path : ImageFiles(g_image.name + ".", ".seen")) {
    uint64_t header[2] = {};
    FILE* f = std::fopen(path.c_str(), "rb");
    const bool ours = f && std::fread(header, 8, 2, f) == 2 && !std::memcmp(header, "BBFXSEEN", 8) && header[1] == g_image.info.FileId;
    if (f) std::fclose(f);
    if (!ours) drop(path);
  }
  if (removed) std::printf("FEX code cache: %zu files that no run can use removed\n", removed);
}

// Free space where the cache lives and the memory the system can still give, MiB (~0: unknown).
uint64_t FreeDiskMiB(const std::string& dir) {
  struct statvfs v {};
  if (statvfs(dir.c_str(), &v) != 0) return ~0ull;
  return (static_cast<uint64_t>(v.f_bavail) * v.f_frsize) >> 20;
}
uint64_t AvailableMemoryMiB() {
  FILE* f = std::fopen("/proc/meminfo", "r");
  if (!f) return ~0ull;
  char line[128];
  unsigned long long kb = 0;
  bool found = false;
  while (!found && std::fgets(line, sizeof(line), f)) found = std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1;
  std::fclose(f);
  return found ? kb >> 10 : ~0ull;
}
constexpr uint64_t kMinDiskMiB = 512, kMinMemoryMiB = 256;

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size) {
    const ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

// The compiled cache from memory to <cache>.new with every write checked, the trailer, an fsync,
// then the rename: a file under the cache's name is whole. Returns what failed, or null.
const char* WriteCacheFile(int mem, size_t payload) {
  void* data = mmap(nullptr, payload, PROT_READ, MAP_SHARED, mem, 0);
  if (data == MAP_FAILED) return "cannot map the compiled data";
  const std::string tmp = g_image.cache_path + ".new";
  const int fd = open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
  CacheTrailer trailer {};
  std::memcpy(trailer.magic, kTrailerMagic, 8);
  trailer.payload = payload;
  trailer.file_id = g_image.info.FileId;
  trailer.config_id = g_image.config_id;
  const char* error = nullptr;
  if (fd < 0) error = "cannot create the file";
  else if (!WriteAll(fd, data, payload) || !WriteAll(fd, &trailer, sizeof(trailer))) error = "a write failed";
  else if (fsync(fd) != 0) error = "fsync failed";
  if (fd >= 0 && close(fd) != 0 && !error) error = "close failed";
  munmap(data, payload);
  if (!error && std::rename(tmp.c_str(), g_image.cache_path.c_str()) != 0) error = "the rename failed";
  if (error) unlink(tmp.c_str());
  return error;
}

// The safety net's tests, BB_FEX_CACHE_TEST=<test>[@<part of the image's name>]: "cut" (the cache
// file loses its last page before the load: it must be refused and removed), "crash:<seconds>" (a
// fatal signal on a host thread that long after the image load: the guard must record it, the next
// run drop the file, the same crash again turn the cache off), "gencrash" (a fatal signal on the
// compile thread: the cache must be off from the next run on).
bool CacheTest(const char* test, double* argument = nullptr) {
  const char* e = std::getenv("BB_FEX_CACHE_TEST");
  if (!e) return false;
  const std::string s = e;
  const size_t at = s.find('@');
  if (at != std::string::npos && g_image.name.find(s.substr(at + 1)) == std::string::npos) return false;
  const std::string what = s.substr(0, at);
  const size_t n = std::strlen(test);
  if (what.compare(0, n, test) != 0) return false;
  if (what.size() == n) return true;
  if (what[n] != ':') return false;
  if (argument) *argument = std::atof(what.c_str() + n + 1);
  return true;
}

void LoadImageCache() {
  if (struct stat cut {}; CacheTest("cut") && stat(g_image.cache_path.c_str(), &cut) == 0 && cut.st_size > 4096) {
    const int result = truncate(g_image.cache_path.c_str(), cut.st_size - 4096);
    std::printf("FEX code cache: BB_FEX_CACHE_TEST=cut, the file's last page cut off (%d)\n", result);
  }
  const int fd = open(g_image.cache_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::printf("FEX code cache: none for this image yet (%s)\n", Basename(g_image.cache_path));
    return;
  }
  struct stat st {};
  void* file = MAP_FAILED;
  if (fstat(fd, &st) == 0 && st.st_size >= static_cast<off_t>(4096 + sizeof(CacheTrailer)))
    file = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  // Whole, and of this image and build: FEX's LoadCache checks no bounds.
  CacheTrailer trailer {};
  bool whole = false;
  if (file != MAP_FAILED) {
    std::memcpy(&trailer, static_cast<const uint8_t*>(file) + st.st_size - sizeof(trailer), sizeof(trailer));
    whole = !std::memcmp(trailer.magic, kTrailerMagic, 8) && trailer.payload == static_cast<uint64_t>(st.st_size) - sizeof(trailer) &&
            trailer.file_id == g_image.info.FileId && trailer.config_id == g_image.config_id;
  }
  if (!whole) {
    if (file != MAP_FAILED) munmap(file, st.st_size);
    unlink(g_image.cache_path.c_str());
    std::printf("FEX code cache: %s is not a whole cache of this image: removed\n", Basename(g_image.cache_path));
    return;
  }
  // Read ahead: nearly all of the file is used in a run (24 of U3's 27 MiB of code), and it is
  // rarely still in the page cache from the last run.
  madvise(file, st.st_size, MADV_WILLNEED);
  // The guard record first: a crash from here on is this cache's until shown otherwise.
  if (g_image.guard.crash && g_image.guard.runs < 1000) ++g_image.guard.runs;
  if (!WriteGuard()) {
    munmap(file, st.st_size);
    std::printf("FEX code cache: not mapped (its guard record cannot be written)\n");
    return;
  }
  const uint64_t t0 = MonotonicNs();
  auto& cache = g_context->GetCodeCache();
  auto mapped = cache.LoadCache(std::span {static_cast<std::byte*>(file), static_cast<size_t>(trailer.payload)}, g_image.info, g_image.base);
  if (!mapped) {
    munmap(file, st.st_size);
    unlink(g_image.cache_path.c_str());
    std::printf("FEX code cache: %s rejected by FEX: removed\n", Basename(g_image.cache_path));
    return;
  }
  cache.RegisterMappedCodeBuffer(*mapped);
  const FEXCore::ExecutableFileSectionInfo section {g_image.info, g_image.base, g_image.base, g_image.end};
  cache.EnableLoadedSection(nullptr, *mapped, section);
  std::printf("FEX code cache: %u blocks, %.1f MiB of code from %s, mapped in %.1f ms\n", mapped->NumBlocks,
              mapped->CodeBuffer.size_bytes() / 1048576.0, Basename(g_image.cache_path), (MonotonicNs() - t0) / 1e6);
  g_image.mapped = std::move(mapped);
  g_image.armed.store(true, std::memory_order_relaxed);
}

// One compile of the cache from the folded records and every block record of this image: a second
// FEXCore context with the same options, compiling for a cache file (relocatable code), on this
// thread. FEX's SaveData checks none of its writes: it writes into memory (a memfd), and
// WriteCacheFile takes that to disk. `min_new`: with a cache there, fewer new blocks than that wait
// for the next start (a pass compiles all the blocks again: for one or two new ones 180 s into a
// run that was 3 s of work during play - U3's roof, that second at 25 FPS instead of 28).
constexpr size_t kLaterPassBlocks = 256;
void CompileImageCache(size_t min_new) {
  if (!g_image.generate) return;
  const uint64_t t0 = MonotonicNs();
  FlushSeen();
  std::set<uint32_t> blocks = ReadBlockList();
  const size_t listed = blocks.size();
  const auto maps = ImageFiles(g_image.name + ".", ".seen");
  for (const auto& path : maps) ReadSeen(path, blocks);
  struct stat st {};
  const bool have_cache = stat(g_image.cache_path.c_str(), &st) == 0;
  if (blocks.empty() || (have_cache && blocks.size() == listed)) {
    // the other runs' records add nothing to the list: done with them
    if (!blocks.empty())
      for (const auto& path : maps)
        if (path != g_image.map_path) unlink(path.c_str());
    std::printf("FEX code cache: up to date (%zu blocks)\n", blocks.size());
    return;
  }
  if (have_cache && blocks.size() - listed < min_new) {
    std::printf("FEX code cache: %zu new blocks wait for the next start's compile\n", blocks.size() - listed);
    return;
  }
  // Code written after the load (nothing does that now) would be compiled as it is at this moment
  // under a key that names the code as it was at the load.
  if (ImageCodeId() != g_image.info.FileId) {
    g_image.generate = false;
    std::printf("FEX code cache: the image's code was written after its load: no cache from this run\n");
    return;
  }
  const uint64_t disk = FreeDiskMiB(g_image.dir), memory = AvailableMemoryMiB();
  if (disk < kMinDiskMiB || memory < kMinMemoryMiB) {
    std::printf("FEX code cache: not compiled now (%llu MiB of disk free, %llu MiB of memory available)\n",
                (unsigned long long)disk, (unsigned long long)memory);
    return;
  }
  auto ctx = FEXCore::Context::Context::CreateNewContext(g_features);
  ctx->GetCodeCache().InitiateCacheGeneration();
  ctx->SetSignalDelegator(g_signals);
  ctx->SetSyscallHandler(g_syscalls);
  ctx->EnableExitOnHLT();
  if (!ctx->InitCore()) {
    std::printf("FEX code cache: cannot start a compile context\n");
    return;
  }
  auto* thread = ctx->CreateThread(0, 0);
  GuestSegments segments;
  segments.Initialize(thread->CurrentFrame->State);
  const uint64_t max_inst = FEXCore::Config::Get_MAXINST();
  size_t compiled = 0, skipped = 0;
  for (const uint32_t offset : blocks) {
    const uint64_t rip = g_image.base + offset;
    if (!ctx->CheckIfBlockIsCacheable(*thread, rip, max_inst)) {
      ++skipped;
      continue;
    }
    ctx->CompileRIP(thread, rip);
    ++compiled;
  }
  if (CacheTest("gencrash")) {
    volatile int* volatile nowhere = nullptr;
    *nowhere = 1;
  }
  const int mem = static_cast<int>(syscall(__NR_memfd_create, "bbfx-cache", 1u /* MFD_CLOEXEC */));
  const FEXCore::ExecutableFileSectionInfo section {g_image.info, g_image.base, g_image.base, g_image.end};
  const bool saved = mem >= 0 && ctx->GetCodeCache().SaveData(*thread, mem, section, 0);
  const off_t payload = saved ? lseek(mem, 0, SEEK_END) : 0;
  ctx->DestroyThread(thread);
  ctx.reset();
  const char* error = !saved ? "FEX saved nothing" : payload < 4096 ? "FEX's data is too short" : WriteCacheFile(mem, static_cast<size_t>(payload));
  if (mem >= 0) close(mem);
  if (error) {
    std::printf("FEX code cache: %s not written (%s)\n", Basename(g_image.cache_path), error);
    return;
  }
  WriteBlockList(blocks);
  for (const auto& path : maps)
    if (path != g_image.map_path) unlink(path.c_str());
  std::printf("FEX code cache: %zu blocks (%zu new) compiled in %.1f s at idle priority (%zu not cacheable), %.1f MiB -> %s "
              "(%llu MiB of memory available)\n",
              compiled, blocks.size() - listed, (MonotonicNs() - t0) / 1e9, skipped, payload / 1048576.0, Basename(g_image.cache_path),
              (unsigned long long)(memory == ~0ull ? 0 : memory));
  std::fflush(stdout);
}

// The block record's writer: every 10 s at normal priority (an idle-priority thread may not run
// for minutes on a busy device).
void SeenFlushThread() {
  pthread_setname_np(pthread_self(), "FEXCacheSeen");
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    FlushSeen();
  }
}

// The compile thread: idle priority (only cores the game leaves free), at the given seconds after
// the image load.
void CodeCacheThread(std::vector<double> at) {
  t_cache_generator = true;
  pthread_setname_np(pthread_self(), "FEXCacheGen");
  sched_param idle {};
  if (sched_setscheduler(0, SCHED_IDLE, &idle) != 0) setpriority(PRIO_PROCESS, 0, 19);
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < at.size(); ++i) {
    std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(at[i])));
    CompileImageCache(i == 0 ? 0 : kLaterPassBlocks); // the first pass is in a game's boot
  }
}

void JitStatsThread() {
  pthread_setname_np(pthread_self(), "JitStats");
  uint64_t seen[kJitThreads] = {};
  const uint64_t start = MonotonicNs();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const uint64_t blocks = g_jit_blocks.exchange(0), outside = g_jit_outside.exchange(0), ns = g_jit_ns.exchange(0),
                   longest = g_jit_max_ns.exchange(0), over2 = g_jit_over2.exchange(0), over10 = g_jit_over10.exchange(0),
                   pages = g_cache_pages.exchange(0), page_ns = g_cache_page_ns.exchange(0),
                   page_faults = g_cache_page_faults.exchange(0);
    // the two threads that compiled longest in these 5 s
    size_t top[2] = {0, 0};
    uint64_t top_ns[2] = {0, 0};
    const size_t n = std::min(g_jit_nthreads.load(std::memory_order_relaxed), kJitThreads);
    for (size_t i = 0; i < n; ++i) {
      const uint64_t now = g_jit_threads[i].ns.load(std::memory_order_relaxed), d = now - seen[i];
      seen[i] = now;
      if (d > top_ns[0]) {
        top[1] = top[0], top_ns[1] = top_ns[0], top[0] = i, top_ns[0] = d;
      } else if (d > top_ns[1]) {
        top[1] = i, top_ns[1] = d;
      }
    }
    if (!blocks && !pages) continue;
    char most[96] = "";
    if (top_ns[0]) std::snprintf(most, sizeof(most), "; most on %s %.1f ms", g_jit_threads[top[0]].name, top_ns[0] / 1e6);
    if (top_ns[1]) {
      const size_t l = std::strlen(most);
      std::snprintf(most + l, sizeof(most) - l, ", %s %.1f ms", g_jit_threads[top[1]].name, top_ns[1] / 1e6);
    }
    std::printf("JIT stats at %.0f s over 5.0 s: %llu blocks compiled (%llu outside the image), %.1f ms, the longest %.2f ms, "
                "%llu over 2 ms, %llu over 10 ms%s; code cache pages loaded %llu in %.1f ms (%llu faults)\n",
                (MonotonicNs() - start) / 1e9, (unsigned long long)blocks, (unsigned long long)outside, ns / 1e6, longest / 1e6,
                (unsigned long long)over2, (unsigned long long)over10, most, (unsigned long long)pages, page_ns / 1e6,
                (unsigned long long)page_faults);
    std::fflush(stdout);
  }
}

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
// arguments, when it was entered and left, the integer result. Written to <file> (HleRecord's) with
// the functions' names in <file>.names and the threads' in <file>.threads ("<id> <name>" lines);
// read by scripts/g2/hle_trace.py (what blocks which thread how long; the frame pacing).
// BB_HLE_TRACE_RING=1 with BB_HLE_TRACE_FILE instead: a flight recorder for hangs, the last 1M calls
// (a few seconds) kept from the start and written when the stall report (probe.c) runs.

// Writes the records of `calls` calls (the ring's last ones, oldest first, once it wrapped).
bool HleTraceWrite(const std::string& file, size_t calls) {
  const size_t n = std::min(calls, kHleMaxRecords);
  const size_t first = calls > kHleMaxRecords ? calls % kHleMaxRecords : 0;
  bool written = false;
  if (FILE* out = std::fopen(file.c_str(), "wb")) {
    written = std::fwrite(g_hle + first, sizeof(HleRecord), n - first, out) == n - first &&
              std::fwrite(g_hle, sizeof(HleRecord), first, out) == first;
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
  return written;
}

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
    const bool written = HleTraceWrite(file, calls);
    std::printf("Host function trace: %zu calls%s, %s %s\n", calls,
                calls > n ? " (the buffer kept the first 1M)" : "", written ? "written to" : "NOT written to",
                file.c_str());
    std::fflush(stdout);
  }
}

void HleTraceStart() {
  const char* secs = std::getenv("BB_HLE_TRACE");
  const char* file = std::getenv("BB_HLE_TRACE_FILE");
  const char* ring = std::getenv("BB_HLE_TRACE_RING");
  const bool ring_mode = ring && ring[0] == '1';
  if (!file || (!secs && !ring_mode)) return;
  g_hle = static_cast<HleRecord*>(std::calloc(kHleMaxRecords, sizeof(HleRecord)));
  if (!g_hle) return;
  if (ring_mode) {
    g_hle_file = file;
    g_hle_ring = true;
    g_hle_on = true;
    std::printf("Host function trace: the last %zu calls kept, written to %s at a stall report\n",
                kHleMaxRecords, file);
    return;
  }
  std::thread(HleTraceThread, std::atof(secs), std::string(file)).detach();
  std::printf("Host function trace: armed (%s s), trigger file %s.trigger\n", secs, file);
}

} // namespace

extern "C" {

void guest_cpu_hle_trace_save(void) {
  if (!g_hle_ring.load() || !g_hle_on.exchange(false)) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(200)); // calls in flight
  const size_t calls = g_hle_n.load();
  const bool written = HleTraceWrite(g_hle_file, calls);
  std::printf("Host function trace: the last %zu of %zu calls %s %s\n", std::min(calls, kHleMaxRecords),
              calls, written ? "written to" : "NOT written to", g_hle_file.c_str());
  std::fflush(stdout);
}

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
  // g2-arm64: FEX's code cache (on unless BB_FEX_CODE_CACHE=0, guest_cpu_code_cache) and the JIT
  // stats. FEXCore reads these when the context is made: a 128 MiB code buffer from the start
  // (growing it would drop the cached blocks), cache pages finalized on their first use.
  g_jit_stats = EnvOn("BB_JIT_STATS");
  if (EnvOff("BB_FEX_CODE_CACHE")) {
    std::printf("FEX code cache: off (BB_FEX_CODE_CACHE=0)\n");
  } else if (sysconf(_SC_PAGESIZE) != 4096) {
    std::printf("FEX code cache: off (this kernel's pages are %ld bytes, FEX's code pages 4096)\n", sysconf(_SC_PAGESIZE));
  } else if (!KernelHasDontUnmap()) {
    std::printf("FEX code cache: off (this kernel has no MREMAP_DONTUNMAP: Linux 5.7 and later)\n");
  } else {
    g_image.on = true;
    g_image.load = !EnvOff("BB_FEX_CACHE_LOAD");
    g_image.generate = !EnvOff("BB_FEX_CACHE_GEN");
    g_image.config_id = CodeCacheConfigId();
    if (const char* chunk = std::getenv("BB_FEX_CACHE_CHUNK")) g_cache_chunk = std::clamp(std::atoi(chunk), 1, 256);
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLECODECACHINGWIP, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLELAZYCODECACHINGWIP, "1");
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

void guest_cpu_code_cache(const char* image_path, const void* image, uint64_t size, const uint64_t (*code)[2], size_t count,
                          const uint64_t* varying, size_t nvarying, int stable) {
  if (!g_context || (!g_image.on && !g_jit_stats)) return;
  g_image.base = reinterpret_cast<uintptr_t>(image);
  g_image.end = g_image.base + size;
  // The run-time values in the code (probe.c's note_varying: imports, host addresses in patches):
  // left out of the hash, and FEX caches no block that reads one as an immediate (a Skip
  // relocation at its first byte, and at the upper half for a 4-byte immediate).
  g_image.slots.assign(varying, varying + nvarying);
  std::sort(g_image.slots.begin(), g_image.slots.end());
  g_image.slots.erase(std::unique(g_image.slots.begin(), g_image.slots.end()), g_image.slots.end());
  for (const uint64_t s : g_image.slots) {
    if (s + 8 > size || s + 4 > 0xffffffffull) continue;
    g_image.info.Relocations[static_cast<uint32_t>(s)] = FEXCore::GuestRelocationType::Skip;
    g_image.info.Relocations[static_cast<uint32_t>(s + 4)] = FEXCore::GuestRelocationType::Skip;
  }
  for (size_t i = 0; i < count; ++i) g_image.code.emplace_back(code[i][0], code[i][1]);
  const uint64_t id = ImageCodeId();
  g_image.info.FileId = id;
  g_image.name = ImageName(image_path);
  g_image.info.Filename = g_image.name.c_str();
  if (g_image.on) {
    g_image.record = true;
    g_image.dir = CodeCacheDir();
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "-%016llx-%016llx.fexcache", (unsigned long long)id, (unsigned long long)g_image.config_id);
    g_image.cache_path = g_image.dir + g_image.name + suffix;
    g_image.map_path = g_image.dir + g_image.name + "." + std::to_string(getpid()) + ".seen";
    g_image.blocks_path = g_image.dir + g_image.name + ".blocks";
    // BB_FEX_CACHE_CLEAR=1: this image's files go (the other images' stay: the menu's run before)
    if (EnvOn("BB_FEX_CACHE_CLEAR")) {
      size_t removed = 0;
      for (const char* prefix : {".", "-"})
        for (const auto& path : ImageFiles(g_image.name + prefix, ""))
          if (unlink(path.c_str()) == 0) ++removed;
      std::printf("FEX code cache: BB_FEX_CACHE_CLEAR, %zu files of this image removed\n", removed);
    }
    std::printf("FEX code cache: image %s, code %016llx (%zu run-time values in it left out), options %016llx (load %d, compile %d)\n",
                g_image.name.c_str(), (unsigned long long)id, g_image.slots.size(), (unsigned long long)g_image.config_id, g_image.load,
                g_image.generate);
    if (!stable) {
      // probe.c placed a thunk by search (its arena was taken or full): the rel32 jumps to it in
      // the image's code, and so this hash, are not the next run's. The files stay as they are.
      g_image.load = g_image.generate = g_image.record = false;
      std::printf("FEX code cache: off in this run (a thunk outside the arena: this image's code is not the next run's)\n");
    } else {
      RemoveStaleFiles();
      OpenGuard();
      if (const uint64_t disk = FreeDiskMiB(g_image.dir); g_image.record && disk < kMinDiskMiB) {
        g_image.generate = g_image.record = false;
        std::printf("FEX code cache: nothing recorded or compiled in this run (%llu MiB of disk free)\n", (unsigned long long)disk);
      }
      if (g_image.load) LoadImageCache();
    }
  }
  // FEX records each compiled block through the code map writer (the JIT stats time them there).
  g_context->SetCodeMapWriter(fextl::make_unique<FEXCore::CodeMapWriter>(g_code_map_file));
  if (g_image.record) std::thread(SeenFlushThread).detach();
  if (g_image.on && g_image.generate) {
    std::vector<double> at;
    const char* list = std::getenv("BB_FEX_CACHE_GEN_DELAY");
    for (const char* p = list && *list ? list : "30,180"; *p;) {
      char* next = nullptr;
      const double s = std::strtod(p, &next);
      if (next == p) break;
      if (s >= 0) at.push_back(s);
      p = *next == ',' ? next + 1 : next;
    }
    std::sort(at.begin(), at.end());
    std::thread(CodeCacheThread, std::move(at)).detach();
  }
  if (g_jit_stats) std::thread(JitStatsThread).detach();
  if (double seconds = 0; g_image.on && CacheTest("crash", &seconds)) {
    std::printf("FEX code cache: BB_FEX_CACHE_TEST=crash, a fatal signal in %.0f s\n", seconds);
    std::thread([seconds] {
      pthread_setname_np(pthread_self(), "FEXCacheTest");
      std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
      volatile int* volatile nowhere = nullptr;
      *nowhere = 1;
    }).detach();
  }
  std::fflush(stdout);
}

// g2-arm64: a fatal signal (probe.c's fault handler, the process is ending) while a cache is
// mapped, or on the thread that compiles one: recorded for the next run (CacheGuard). Atomics,
// pwrite and dladdr only.
void guest_cpu_code_cache_crashed(int sig, void* context) {
  const bool compiling = t_cache_generator;
  if (g_image.guard_fd < 0 || (!compiling && !g_image.armed.load(std::memory_order_relaxed))) return;
  if (g_image.crash_noted.exchange(true, std::memory_order_relaxed)) return;
  uint64_t where = 0;
  if (context) {
    const auto pc = static_cast<uint64_t>(static_cast<const ucontext_t*>(context)->uc_mcontext.pc);
    Dl_info module {};
    if (t_host.fex && g_context->IsAddressInCodeBuffer(t_host.fex, pc)) where = g_context->RestoreRIPFromHostPC(t_host.fex, pc);
    else if (dladdr(reinterpret_cast<void*>(pc), &module) && module.dli_fbase) where = pc - reinterpret_cast<uintptr_t>(module.dli_fbase);
    else where = pc & 0xfff;
  }
  CacheGuard g = g_image.guard;
  const uint64_t crash = HashMix(HashMix(0x6372617368ull, static_cast<uint64_t>(sig)), where) | 1;
  if (compiling || (g.crash == crash && g.runs <= 3)) {
    g.state = kGuardOff;
    g.crash = crash;
  } else {
    g.state = kGuardSuspect;
    g.crash = crash;
    g.runs = 0;
  }
  g_image.guard = g;
  WriteGuard();
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
  // g2-arm64: a code cache page's first use (guest_cpu_code_cache): its code is copied from the file,
  // relocated and made executable, as FEX's Linux frontend does in its fault handler.
  if (sig == SIGSEGV && info && g_image.mapped) {
    auto& code = *g_image.mapped;
    const auto begin = reinterpret_cast<uintptr_t>(code.CodeBuffer.data());
    const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    if (address - begin < code.CodeBuffer.size_bytes()) {
      const uint64_t t0 = MonotonicNs();
      {
        std::lock_guard lock {g_image.finalize};
        const size_t page = (address - begin) / FEXCore::Utils::FEX_PAGE_SIZE;
        // With the page its neighbours that are not in use yet, inside its chunk (16 pages): one
        // fault, one remap and one TLB flush for them all. A page at a time took 51 us each: 314 ms
        // over the threads of a warm U3 run (6,141 pages). Only pages not loaded are given: FEX
        // finalizes a page once (it adds the pages a relocation crosses into, loaded together).
        size_t first = page, last = page + 1;
        if (!code.LoadedPages[page]) {
          const size_t chunk = page - page % g_cache_chunk;
          const size_t end = std::min(chunk + g_cache_chunk, code.CodeBuffer.size_bytes() / FEXCore::Utils::FEX_PAGE_SIZE);
          while (first > chunk && !code.LoadedPages[first - 1]) --first;
          while (last < end && !code.LoadedPages[last]) ++last;
        }
        const auto range = FEXCore::AbstractCodeCache::SelectCodeRangeToFinalize(code, first, last);
        if (!range.empty()) {
          g_context->GetCodeCache().FinalizeCodePages(code, range);
          g_cache_pages.fetch_add(range.size_bytes() / FEXCore::Utils::FEX_PAGE_SIZE, std::memory_order_relaxed);
        }
      }
      g_cache_page_ns.fetch_add(MonotonicNs() - t0, std::memory_order_relaxed);
      g_cache_page_faults.fetch_add(1, std::memory_order_relaxed);
      return 1;
    }
  }
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

int guest_cpu_describe_fault(char* out, size_t size, const void* context) {
  HostThread& t = t_host;
  if (!t.fex || !context || size < 2048) return 0;
  const auto* uc = static_cast<const ucontext_t*>(context);
  const auto pc = static_cast<uint64_t>(uc->uc_mcontext.pc);
  if (!g_context->IsAddressInCodeBuffer(t.fex, pc)) return 0;
  // While translated code runs FEX keeps rax..r15 in fixed host registers (x64::SRA in
  // Arm64Emitter.cpp); the frame state is only the last sync.
  static constexpr int kHost[16] = {4, 7, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 29};
  static const char* const names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  uint64_t g[16];
  for (int r = 0; r < 16; ++r) g[r] = uc->uc_mcontext.regs[kHost[r]];
  const uint64_t rip = g_context->RestoreRIPFromHostPC(t.fex, pc);
  size_t n = (size_t)std::snprintf(out, size, "  translated code: guest rip %#llx, its registers:\n",
                                   (unsigned long long)rip);
  for (int r = 0; r < 16 && n + 100 < size; r += 4) {
    n += (size_t)std::snprintf(out + n, size - n, "  ");
    for (int k = r; k < r + 4; ++k)
      n += (size_t)std::snprintf(out + n, size - n, " %-3s=%016llx", names[k], (unsigned long long)g[k]);
    n += (size_t)std::snprintf(out + n, size - n, "\n");
  }
  uint8_t code[16];
  if (ProfRead(code, rip, sizeof(code))) {
    n += (size_t)std::snprintf(out + n, size - n, "  code at that rip:");
    for (uint8_t b : code) n += (size_t)std::snprintf(out + n, size - n, " %02x", b);
    n += (size_t)std::snprintf(out + n, size - n, "\n");
  }
  // The return addresses up the rbp chain (frame pointers are kept in the games' code).
  n += (size_t)std::snprintf(out + n, size - n, "  rbp chain:");
  for (uint64_t rbp = g[5], i = 0; i < 24 && n + 40 < size; ++i) {
    uint64_t frame[2];
    if ((rbp & 7) || !ProfRead(frame, rbp, sizeof(frame))) break;
    n += (size_t)std::snprintf(out + n, size - n, " %#llx", (unsigned long long)frame[1]);
    if (frame[0] <= rbp || frame[0] - rbp > (1u << 20)) break;
    rbp = frame[0];
  }
  n += (size_t)std::snprintf(out + n, size - n, "\n");
  // The live stack: the frame from rsp to rbp+0x10 in full, then the image addresses above it.
  uint64_t words[384];
  struct iovec local = {words, sizeof(words)};
  struct iovec remote = {reinterpret_cast<void*>(g[4]), sizeof(words)};
  const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  const size_t nw = got > 0 ? (size_t)got / 8 : 0;
  size_t fw = 0;
  if (g[5] > g[4] && g[5] - g[4] <= 0x300) fw = std::min(nw, (size_t)((g[5] - g[4]) / 8 + 2));
  for (size_t i = 0; i < fw && n + 100 < size; i += 4) {
    n += (size_t)std::snprintf(out + n, size - n, "  [rsp+0x%03zx]", i * 8);
    for (size_t k = i; k < i + 4 && k < fw; ++k)
      n += (size_t)std::snprintf(out + n, size - n, " %016llx", (unsigned long long)words[k]);
    n += (size_t)std::snprintf(out + n, size - n, "\n");
  }
  n += (size_t)std::snprintf(out + n, size - n, "  live stack image addresses:");
  for (size_t i = fw, shown = 0; i < nw && shown < 48 && n + 32 < size; ++i) {
    if (words[i] < 0x400000 || words[i] >= 0x80000000ull) continue;
    n += (size_t)std::snprintf(out + n, size - n, " +%#zx=%#llx", i * 8, (unsigned long long)words[i]);
    shown++;
  }
  n += (size_t)std::snprintf(out + n, size - n, "\n");
  // 64 bytes at every register that points into guest memory (heap, stacks), once per value.
  for (int r = 0; r < 16; ++r) {
    const uint64_t v = g[r];
    if (r == 4 || r == 5 || v < 0x1000000000ull || v >= 0x4000000000ull) continue;
    bool seen = false;
    for (int k = 0; k < r; ++k) seen |= g[k] == v && k != 4 && k != 5;
    uint64_t m[8];
    if (seen || !ProfRead(m, v, sizeof(m))) continue;
    for (int i = 0; i < 8 && n + 100 < size; i += 4)
      n += (size_t)std::snprintf(out + n, size - n, "  [%s+%#04x] %016llx %016llx %016llx %016llx\n", names[r], i * 8,
                                 (unsigned long long)m[i], (unsigned long long)m[i + 1], (unsigned long long)m[i + 2],
                                 (unsigned long long)m[i + 3]);
  }
  return (int)n;
}

} // extern "C"
