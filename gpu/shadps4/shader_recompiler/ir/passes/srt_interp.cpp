// SPDX-FileCopyrightText: Copyright 2026 uncharted-g2
// SPDX-License-Identifier: GPL-2.0-or-later

// g2-arm64: SRT walkers are x86-64 code (Xbyak, flatten_extended_userdata_pass.cpp). On ARM64
// hosts they are interpreted here instead of running on the guest CPU (FEXCore):
//  - shadPS4 lets a walker fault on a stale or garbage pointer and patches the faulting
//    `mov reg, [mem]` into `xor reg, reg` (SrtWalkerSignalHandler); under FEX the fault happens
//    in translated code that handler does not recognise, and the game crashed while loading;
//  - entering the JIT per draw costs more than interpreting a few dozen instructions.
// A walker is straight-line code from a small set of instructions (no branches, one ret); it is
// decoded once with Zydis and cached. Its loads run natively under runtime_fault_recover: a load
// that faults is marked to give 0 from then on (shadPS4's patch) and the walk restarts.
// Runs of dword copies (`mov r10d, [rdi + a]; mov [rsi + b], r10d` with consecutive offsets: the
// sharps of a table level and the bbport constant windows of 256 dwords) are one block copy.

#if defined(__aarch64__)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <Zydis/Zydis.h>

#include "common/decoder.h"
#include "shader_recompiler/ir/passes/srt.h"

extern "C" __thread sigjmp_buf* runtime_fault_recover; // ../src/runtime_memory.c

namespace Shader {
namespace {

enum class Kind : u8 { Mov, Add, Sub, Imul, And, Or, Xor, Shl, Shr, Cmp, Cmova, Cmovb, Dec, Neg, Not, Push, Pop, Ret, Copy };

constexpr u8 NoReg = 0xff;

struct Operand {
    enum Type : u8 { None, Reg, Imm, Mem } type = None;
    u8 reg = NoReg; // Reg: register index (rax = 0 ... r15 = 15)
    u8 size = 8;    // Reg/Mem: bytes
    u8 base = NoReg, index = NoReg, scale = 1;
    s64 value = 0; // Imm: value; Mem: displacement
};

// Copy: ops[0] the first destination dword, ops[1] the first source dword, ops[2] the register
// the pairs load through (it ends with the last dword) with the dword count in value; the pairs
// follow it and are skipped while it is in use.
struct Op {
    Kind kind;
    u8 count;
    // A load that faulted: the destination gets 0 (shadPS4). A copy that faulted: not used any
    // more, its pairs run one by one.
    std::atomic<bool> zeroed{false};
    // g2-arm64: a Mov whose load faulted reads 0 while the walk count is below this - not for
    // good: a table the game freed or had not mapped yet is tried again after ZeroWalks walks.
    std::atomic<u64> zero_until{0};
    Operand ops[3];
    Op(Kind k, u8 n) : kind{k}, count{n} {}
    Op(Op&& o) noexcept
        : kind{o.kind}, count{o.count}, zeroed{o.zeroed.load()}, zero_until{o.zero_until.load()} {
        std::memcpy(ops, o.ops, sizeof(ops));
    }
};

struct Program {
    uintptr_t key = 0; // the walker
    std::vector<Op> ops;
    bool supported = true;
    u32 copies = 0, copy_dwords = 0, single_ops = 0; // per walk, for BB_SRT_STATS
    u32 flat_bytes = 0; // bytes of the flat buffer it writes (BB_SRT_CHECK); 0: not checked
};

struct Settings {
    bool fex = std::getenv("BB_SRT_FEX") != nullptr;
    bool copies = !(std::getenv("BB_SRT_COPY") && std::getenv("BB_SRT_COPY")[0] == '0');
    bool check = std::getenv("BB_SRT_CHECK") != nullptr;
    bool stats = std::getenv("BB_SRT_STATS") != nullptr;
};

const Settings& GetSettings() {
    static const Settings settings;
    return settings;
}

std::shared_mutex g_programs_mutex;
std::unordered_map<uintptr_t, std::unique_ptr<Program>> g_programs;

u8 RegIndex(ZydisRegister reg) {
    const ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (full < ZYDIS_REGISTER_RAX || full > ZYDIS_REGISTER_R15) {
        return NoReg;
    }
    return static_cast<u8>(full - ZYDIS_REGISTER_RAX);
}

bool Convert(const ZydisDecodedOperand& in, Operand& out) {
    switch (in.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        out.type = Operand::Reg;
        out.reg = RegIndex(in.reg.value);
        out.size = static_cast<u8>(in.size / 8);
        return out.reg != NoReg && (out.size == 4 || out.size == 8);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        out.type = Operand::Imm;
        out.value = in.imm.is_signed ? in.imm.value.s : static_cast<s64>(in.imm.value.u);
        return true;
    case ZYDIS_OPERAND_TYPE_MEMORY:
        out.type = Operand::Mem;
        out.size = static_cast<u8>(in.size / 8);
        out.base = in.mem.base == ZYDIS_REGISTER_NONE ? NoReg : RegIndex(in.mem.base);
        out.index = in.mem.index == ZYDIS_REGISTER_NONE ? NoReg : RegIndex(in.mem.index);
        out.scale = in.mem.scale ? in.mem.scale : 1;
        out.value = in.mem.disp.has_displacement ? in.mem.disp.value : 0;
        return (out.size == 4 || out.size == 8) &&
               (in.mem.base == ZYDIS_REGISTER_NONE || out.base != NoReg) &&
               (in.mem.index == ZYDIS_REGISTER_NONE || out.index != NoReg);
    default:
        return false;
    }
}

std::unique_ptr<Program> Decode(const u8* code) {
    auto program = std::make_unique<Program>();
    for (int guard = 0; guard < 1 << 20; ++guard) {
        ZydisDecodedInstruction inst;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                inst, operands, const_cast<u8*>(code), 15))) {
            program->supported = false;
            break;
        }
        Kind kind;
        switch (inst.mnemonic) {
        case ZYDIS_MNEMONIC_MOV: kind = Kind::Mov; break;
        case ZYDIS_MNEMONIC_ADD: kind = Kind::Add; break;
        case ZYDIS_MNEMONIC_SUB: kind = Kind::Sub; break;
        case ZYDIS_MNEMONIC_IMUL: kind = Kind::Imul; break;
        case ZYDIS_MNEMONIC_AND: kind = Kind::And; break;
        case ZYDIS_MNEMONIC_OR: kind = Kind::Or; break;
        case ZYDIS_MNEMONIC_XOR: kind = Kind::Xor; break;
        case ZYDIS_MNEMONIC_SHL: kind = Kind::Shl; break;
        case ZYDIS_MNEMONIC_SHR: kind = Kind::Shr; break;
        case ZYDIS_MNEMONIC_CMP: kind = Kind::Cmp; break;
        case ZYDIS_MNEMONIC_CMOVNBE: kind = Kind::Cmova; break;
        case ZYDIS_MNEMONIC_CMOVB: kind = Kind::Cmovb; break;
        case ZYDIS_MNEMONIC_DEC: kind = Kind::Dec; break;
        case ZYDIS_MNEMONIC_NEG: kind = Kind::Neg; break;
        case ZYDIS_MNEMONIC_NOT: kind = Kind::Not; break;
        case ZYDIS_MNEMONIC_PUSH: kind = Kind::Push; break;
        case ZYDIS_MNEMONIC_POP: kind = Kind::Pop; break;
        case ZYDIS_MNEMONIC_RET: kind = Kind::Ret; break;
        case ZYDIS_MNEMONIC_NOP: code += inst.length; continue; // patched-out bytes
        default:
            std::printf("SRT interpreter: unsupported instruction %s at %p\n",
                        ZydisMnemonicGetString(inst.mnemonic), static_cast<const void*>(code));
            program->supported = false;
            return program;
        }
        Op op{kind, 0};
        if (kind != Kind::Ret && kind != Kind::Push && kind != Kind::Pop) {
            for (u8 i = 0; i < inst.operand_count_visible && i < 3; ++i) {
                if (!Convert(operands[i], op.ops[i])) {
                    std::printf("SRT interpreter: unsupported operand in %s at %p\n",
                                ZydisMnemonicGetString(inst.mnemonic),
                                static_cast<const void*>(code));
                    program->supported = false;
                    return program;
                }
                op.count = i + 1;
            }
        } else if (kind != Kind::Ret) {
            if (!Convert(operands[0], op.ops[0]) || op.ops[0].type != Operand::Reg) {
                program->supported = false;
                return program;
            }
            op.count = 1;
        }
        program->ops.push_back(std::move(op));
        if (kind == Kind::Ret) {
            break;
        }
        code += inst.length;
    }
    return program;
}

constexpr u8 Rsi = 6;

bool IsCopyLoad(const Op& op) {
    const Operand& d = op.ops[0];
    const Operand& s = op.ops[1];
    return op.kind == Kind::Mov && op.count == 2 && d.type == Operand::Reg && d.size == 4 &&
           s.type == Operand::Mem && s.size == 4 && s.base != NoReg && s.index == NoReg &&
           s.base != d.reg;
}

bool IsCopyStore(const Op& op, u8 reg, u8 src_base) {
    const Operand& d = op.ops[0];
    const Operand& s = op.ops[1];
    return op.kind == Kind::Mov && op.count == 2 && d.type == Operand::Mem && d.size == 4 &&
           d.base != NoReg && d.index == NoReg && d.base != reg && d.base != src_base &&
           s.type == Operand::Reg && s.size == 4 && s.reg == reg;
}

// Puts a Copy before each run of two or more copy pairs with consecutive offsets.
void FuseCopies(Program& p) {
    auto& in = p.ops;
    std::vector<Op> out;
    out.reserve(in.size() + in.size() / 4);
    for (size_t i = 0; i < in.size();) {
        size_t pairs = 0;
        if (i + 1 < in.size() && IsCopyLoad(in[i]) &&
            IsCopyStore(in[i + 1], in[i].ops[0].reg, in[i].ops[1].base)) {
            const u8 reg = in[i].ops[0].reg;
            const Operand src = in[i].ops[1];
            const Operand dst = in[i + 1].ops[0];
            pairs = 1;
            while (i + 2 * pairs + 1 < in.size()) {
                const Op& load = in[i + 2 * pairs];
                const Op& store = in[i + 2 * pairs + 1];
                const s64 step = static_cast<s64>(4 * pairs);
                if (!IsCopyLoad(load) || load.ops[0].reg != reg || load.ops[1].base != src.base ||
                    load.ops[1].value != src.value + step || !IsCopyStore(store, reg, src.base) ||
                    store.ops[0].base != dst.base || store.ops[0].value != dst.value + step) {
                    break;
                }
                ++pairs;
            }
            if (pairs >= 2) {
                Op copy{Kind::Copy, 3};
                copy.ops[0] = dst;
                copy.ops[1] = src;
                copy.ops[2].type = Operand::Reg;
                copy.ops[2].reg = reg;
                copy.ops[2].size = 4;
                copy.ops[2].value = static_cast<s64>(pairs);
                out.push_back(std::move(copy));
                ++p.copies;
                p.copy_dwords += static_cast<u32>(pairs);
            }
        }
        const size_t end = i + (pairs >= 2 ? 2 * pairs : 1);
        for (; i < end; ++i) {
            p.single_ops += pairs >= 2 ? 0 : 1;
            out.push_back(std::move(in[i]));
        }
    }
    in = std::move(out);
}

// The flat buffer bytes a program writes, when it writes only at fixed offsets from rsi.
u32 FlatBytes(const Program& p) {
    u32 bytes = 0;
    for (const Op& op : p.ops) {
        const Operand& d = op.ops[0];
        if (op.kind == Kind::Push || op.kind == Kind::Pop || op.kind == Kind::Ret ||
            d.type != Operand::Mem) {
            continue;
        }
        if (d.base != Rsi || d.index != NoReg || d.value < 0) {
            return 0;
        }
        bytes = std::max(bytes, static_cast<u32>(d.value) + d.size);
    }
    return bytes;
}

std::unique_ptr<Program> Prepare(const u8* code) {
    auto program = Decode(code);
    if (program->supported) {
        program->flat_bytes = FlatBytes(*program);
        if (GetSettings().copies) {
            FuseCopies(*program);
        } else {
            program->single_ops = static_cast<u32>(program->ops.size());
        }
    }
    return program;
}

Program* GetProgram(PFN_SrtWalker walker) {
    const auto key = reinterpret_cast<uintptr_t>(walker);
    // A lock-free cache in front of the shared map: its lock was taken on every walk (several
    // thousand per frame), and a thread_local cache costs a call to the dynamic TLS resolver
    // (libbbgpu is loaded with dlopen). Programs are never freed.
    static std::array<std::atomic<Program*>, 256> cache{};
    auto& entry = cache[(key * 0x9E3779B97F4A7C15ull) >> 56];
    if (Program* cached = entry.load(std::memory_order_acquire); cached && cached->key == key) {
        return cached;
    }
    Program* program = nullptr;
    {
        std::shared_lock lk{g_programs_mutex};
        if (auto it = g_programs.find(key); it != g_programs.end()) {
            program = it->second.get();
        }
    }
    if (!program) {
        std::unique_lock lk{g_programs_mutex};
        auto& slot = g_programs[key];
        if (!slot) {
            slot = Prepare(reinterpret_cast<const u8*>(walker));
            slot->key = key;
        }
        program = slot.get();
    }
    entry.store(program, std::memory_order_release);
    return program;
}

struct State {
    u64 r[16];
    bool cf, zf;
};

inline u64 Mask(u64 v, u8 size) {
    return size == 4 ? (v & 0xffffffffull) : v;
}

inline uintptr_t Address(const Operand& o, const State& s) {
    u64 a = static_cast<u64>(o.value);
    if (o.base != NoReg) a += s.r[o.base];
    if (o.index != NoReg) a += s.r[o.index] * o.scale;
    return static_cast<uintptr_t>(a);
}

inline u64 Read(const Operand& o, const State& s, u8 size) {
    switch (o.type) {
    case Operand::Reg:
        return Mask(s.r[o.reg], o.size);
    case Operand::Imm:
        return Mask(static_cast<u64>(o.value), size);
    case Operand::Mem: {
        const auto* p = reinterpret_cast<const void*>(Address(o, s));
        if (o.size == 4) {
            u32 v;
            std::memcpy(&v, p, 4);
            return v;
        }
        u64 v;
        std::memcpy(&v, p, 8);
        return v;
    }
    default:
        return 0;
    }
}

inline void Write(const Operand& o, State& s, u64 v) {
    if (o.type == Operand::Reg) {
        s.r[o.reg] = Mask(v, o.size); // 32-bit writes zero-extend
    } else if (o.type == Operand::Mem) {
        auto* p = reinterpret_cast<void*>(Address(o, s));
        if (o.size == 4) {
            const u32 w = static_cast<u32>(v);
            std::memcpy(p, &w, 4);
        } else {
            std::memcpy(p, &v, 8);
        }
    }
}

// current_op: the op running, for the fault handler. On the caller's stack: a thread_local
// cost a call to the dynamic TLS resolver per op.
// g2-arm64: walks of every walker, counted; a faulting load's zero expires after ZeroWalks of them
// (~20 ms at the chase's 200k walks/s).
std::atomic<u64> g_walks{0};
constexpr u64 ZeroWalks = 4096;

void Execute(Program& p, const u32* user_data, u32* flat_dst, bool copies,
             volatile size_t& current_op, u64 walk) {
    u64 stack[64];
    State s{};
    s.r[7] = reinterpret_cast<u64>(user_data); // rdi
    s.r[6] = reinterpret_cast<u64>(flat_dst);  // rsi
    s.r[4] = reinterpret_cast<u64>(&stack[56]); // rsp (headroom above, as natively)
    for (size_t i = 0; i < p.ops.size(); ++i) {
        current_op = i;
        Op& op = p.ops[i];
        const Operand& d = op.ops[0];
        const Operand& a = op.ops[1];
        const u8 size = d.type == Operand::None ? 8 : d.size;
        switch (op.kind) {
        case Kind::Mov:
            Write(d, s, op.zero_until.load(std::memory_order_relaxed) > walk ? 0 : Read(a, s, size));
            break;
        case Kind::Add: Write(d, s, Read(d, s, size) + Read(a, s, size)); break;
        case Kind::Sub: Write(d, s, Read(d, s, size) - Read(a, s, size)); break;
        case Kind::Imul:
            if (op.count == 3) {
                Write(d, s, Read(a, s, size) * Read(op.ops[2], s, size));
            } else {
                Write(d, s, Read(d, s, size) * Read(a, s, size));
            }
            break;
        case Kind::And: Write(d, s, Read(d, s, size) & Read(a, s, size)); break;
        case Kind::Or: Write(d, s, Read(d, s, size) | Read(a, s, size)); break;
        case Kind::Xor: Write(d, s, Read(d, s, size) ^ Read(a, s, size)); break;
        case Kind::Shl:
        case Kind::Shr: {
            const u64 count = Read(a, s, 1) & (size == 8 ? 63 : 31);
            const u64 v = Read(d, s, size);
            Write(d, s, op.kind == Kind::Shl ? v << count : v >> count);
            break;
        }
        case Kind::Cmp: {
            const u64 x = Read(d, s, size), y = Read(a, s, size);
            s.cf = x < y;
            s.zf = x == y;
            break;
        }
        case Kind::Cmova:
        case Kind::Cmovb: {
            const bool take = op.kind == Kind::Cmova ? (!s.cf && !s.zf) : s.cf;
            Write(d, s, take ? Read(a, s, size) : Read(d, s, size));
            break;
        }
        case Kind::Dec: Write(d, s, Read(d, s, size) - 1); break;
        case Kind::Neg: Write(d, s, 0 - Read(d, s, size)); break;
        case Kind::Not: Write(d, s, ~Read(d, s, size)); break;
        case Kind::Push:
            s.r[4] -= 8;
            std::memcpy(reinterpret_cast<void*>(s.r[4]), &s.r[d.reg], 8);
            break;
        case Kind::Pop:
            std::memcpy(&s.r[d.reg], reinterpret_cast<const void*>(s.r[4]), 8);
            s.r[4] += 8;
            break;
        case Kind::Ret:
            return;
        case Kind::Copy:
            if (copies && !op.zeroed.load(std::memory_order_relaxed)) {
                const u64 dwords = static_cast<u64>(op.ops[2].value);
                auto* dst = reinterpret_cast<u8*>(Address(d, s));
                std::memcpy(dst, reinterpret_cast<const void*>(Address(a, s)), dwords * 4);
                u32 last;
                std::memcpy(&last, dst + (dwords - 1) * 4, 4);
                s.r[op.ops[2].reg] = last;
                i += 2 * dwords; // its pairs
            }
            break;
        }
    }
}

// BB_SRT_CHECK: the walk without block copies (every pair on its own, as before them) into a
// copy of the flat buffer as it was before the walk; a fault there only skips the comparison.
void CheckWalk(Program& p, PFN_SrtWalker walker, const u32* user_data, const u32* flat_dst,
               std::vector<u32>& reference, u64 current_walk) {
    sigjmp_buf recover;
    sigjmp_buf* const saved = runtime_fault_recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = saved;
        return;
    }
    runtime_fault_recover = &recover;
    volatile size_t current_op = 0;
    Execute(p, user_data, reference.data(), false, current_op, current_walk);
    runtime_fault_recover = saved;
    static std::atomic<u64> walks{0}, mismatches{0};
    const u64 walk = walks.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t dwords = p.flat_bytes / 4;
    for (size_t i = 0; i < dwords; ++i) {
        if (reference[i] != flat_dst[i]) {
            if (mismatches.fetch_add(1, std::memory_order_relaxed) < 32) {
                std::printf("SRT check: walker %p dword %zu is %08x, without block copies %08x\n",
                            reinterpret_cast<void*>(walker), i, flat_dst[i], reference[i]);
            }
            break;
        }
    }
    if ((walk & 0xffff) == 0) {
        std::printf("SRT check: %llu walks compared, %llu differ\n",
                    static_cast<unsigned long long>(walk),
                    static_cast<unsigned long long>(mismatches.load()));
    }
}

void CountWalk(const Program& p) {
    static std::atomic<u64> walks{0}, copies{0}, copy_dwords{0}, single_ops{0};
    static std::atomic<s64> window_start{0};
    walks.fetch_add(1, std::memory_order_relaxed);
    copies.fetch_add(p.copies, std::memory_order_relaxed);
    copy_dwords.fetch_add(p.copy_dwords, std::memory_order_relaxed);
    single_ops.fetch_add(p.single_ops, std::memory_order_relaxed);
    thread_local u32 tick = 0;
    if ((++tick & 1023) != 0) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 start = window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        window_start.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 5000 || !window_start.compare_exchange_strong(start, now)) {
        return;
    }
    const double seconds = double(now - start) / 1000.0;
    const u64 n = std::max<u64>(walks.exchange(0), 1);
    std::printf("SRT walks: %.0f/s; per walk %.1f single ops, %.1f block copies of %.1f dwords\n",
                double(n) / seconds, double(single_ops.exchange(0)) / double(n),
                double(copies.exchange(0)) / double(n), double(copy_dwords.exchange(0)) / double(n));
}

} // namespace

bool RunSrtWalkerInterpreted(PFN_SrtWalker walker, const u32* user_data, u32* flat_dst) {
    const Settings& settings = GetSettings();
    if (settings.fex) {
        return false;
    }
    Program* program = GetProgram(walker);
    if (!program->supported) {
        return false; // the guest CPU runs it
    }
    if (settings.stats) {
        CountWalk(*program);
    }
    const bool check = settings.check && settings.copies && program->copies && program->flat_bytes;
    std::vector<u32>* reference = nullptr;
    if (check) {
        thread_local std::vector<u32> buffer; // its guard only on this path
        buffer.assign(flat_dst, flat_dst + program->flat_bytes / 4);
        reference = &buffer;
    }
    volatile size_t current_op = 0;
    const u64 walk = g_walks.fetch_add(1, std::memory_order_relaxed);
    for (size_t attempt = 0; attempt <= program->ops.size(); ++attempt) {
        sigjmp_buf recover;
        sigjmp_buf* const saved = runtime_fault_recover;
        if (sigsetjmp(recover, 0)) {
            runtime_fault_recover = saved;
            Op& op = program->ops[current_op];
            if (op.kind == Kind::Copy && !op.zeroed.exchange(true)) {
                continue; // its pairs run one by one from now on
            }
            if (op.kind != Kind::Mov || op.zero_until.load(std::memory_order_relaxed) > walk) {
                std::printf("SRT interpreter: walker %p faulted in op %zu (kind %u), giving up\n",
                            reinterpret_cast<void*>(walker), static_cast<size_t>(current_op),
                            unsigned(op.kind));
                std::abort();
            }
            // g2-arm64: the zero was for good once - U3's chase, 2026-11-09: a whole chain of 70
            // loads of one walker faulted at once (a table freed or not yet mapped), every later
            // walk of it fed the recompiler a null buffer, an assertion stopped the process.
            op.zero_until.store(walk + ZeroWalks, std::memory_order_relaxed);
            static std::atomic<u32> reports{0};
            if (reports.fetch_add(1) < 32) {
                std::printf("SRT interpreter: walker %p load %zu faulted; it reads 0 for the next "
                            "%llu walks\n",
                            reinterpret_cast<void*>(walker), static_cast<size_t>(current_op),
                            static_cast<unsigned long long>(ZeroWalks));
            }
            continue;
        }
        runtime_fault_recover = &recover;
        Execute(*program, user_data, flat_dst, settings.copies, current_op, walk);
        runtime_fault_recover = saved;
        if (check && attempt == 0) {
            CheckWalk(*program, walker, user_data, flat_dst, *reference, walk);
        }
        return true;
    }
    return true;
}

} // namespace Shader

#endif
