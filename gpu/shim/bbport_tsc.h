// g2-arm64: cycle counter for the bbport statistics (x86 TSC or the ARM64 virtual counter).
// Only differences of the same counter are used (ratios and per-interval percentages).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#ifdef _WIN32
#include <intrin.h>
#endif
static inline std::uint64_t bb_rdtsc() { return __rdtsc(); }
static inline void bb_cpu_relax() { _mm_pause(); }
#elif defined(__aarch64__)
static inline std::uint64_t bb_rdtsc() {
    std::uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static inline void bb_cpu_relax() { asm volatile("yield"); }
#else
#error "bb_rdtsc: unsupported architecture"
#endif

// g2-arm64: waiting without a system call. On ARM64 an exclusive load arms the core's monitor
// on the word's cache line and wfe sleeps until another core writes that line, an sev, an
// interrupt or the kernel's timer event stream (Linux: 10 kHz) - the core idles instead of
// spinning on yield, and wakes as soon as the line changes. Elsewhere a pause.
// BB_WAIT_WFE=0: yield as before.
static inline bool bb_wfe_enabled() {
#if defined(__aarch64__)
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WAIT_WFE");
        return !(env && env[0] == '0');
    }();
    return enabled;
#else
    return false;
#endif
}

/// One step of waiting while *word holds `value` (8, 4 or 1 bytes). Callers loop and recheck
/// their condition: the wait may end early, and once the line changed for any reason.
template <typename T>
static inline void bb_wait_word(const std::atomic<T>* word, T value) {
#if defined(__aarch64__)
    static_assert(sizeof(T) == 8 || sizeof(T) == 4 || sizeof(T) == 1);
    if (bb_wfe_enabled()) {
        std::uint64_t current;
        if constexpr (sizeof(T) == 8) {
            asm volatile("ldaxr %0, [%1]" : "=r"(current) : "r"(word) : "memory");
        } else if constexpr (sizeof(T) == 4) {
            asm volatile("ldaxr %w0, [%1]" : "=r"(current) : "r"(word) : "memory");
        } else {
            asm volatile("ldaxrb %w0, [%1]" : "=r"(current) : "r"(word) : "memory");
        }
        if (current == static_cast<std::uint64_t>(value)) {
            asm volatile("wfe" ::: "memory");
        }
        return;
    }
#endif
    (void)word;
    (void)value;
    bb_cpu_relax();
}

/// A deadline for a short spin, cheap to check: the ARM64 counter (no vDSO clock call).
class BbSpinDeadline {
public:
    explicit BbSpinDeadline(std::uint64_t microseconds) {
#if defined(__aarch64__)
        end = bb_rdtsc() + microseconds * TicksPerUs();
#else
        end = std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds);
#endif
    }
    [[nodiscard]] bool Passed() const {
#if defined(__aarch64__)
        return bb_rdtsc() >= end;
#else
        return std::chrono::steady_clock::now() >= end;
#endif
    }

private:
#if defined(__aarch64__)
    static std::uint64_t TicksPerUs() {
        static const std::uint64_t ticks = [] {
            std::uint64_t hz;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
            return hz / 1000000 ? hz / 1000000 : 1;
        }();
        return ticks;
    }
    std::uint64_t end;
#else
    std::chrono::steady_clock::time_point end;
#endif
};

/// g2-arm64 diagnosis: BB_TAX_A_NS=<ns> burns that long on the command thread for every draw
/// and dispatch (Rasterizer::PostDraw), BB_TAX_B_NS=<ns> on the recording thread for every
/// packet (DrawPipe::Run, inside its busy time): the frame's sensitivity to a stage's time -
/// what a diet of that stage would buy - measured before the diet is written. Off unless set.
class BbStageTax {
public:
    static std::uint64_t StageA() {
        static const std::uint64_t ns = Read("BB_TAX_A_NS");
        return ns;
    }
    static std::uint64_t StageB() {
        static const std::uint64_t ns = Read("BB_TAX_B_NS");
        return ns;
    }
    static void Pay(std::uint64_t ns) {
        if (ns == 0) {
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
        while (std::chrono::steady_clock::now() < until) {
        }
    }

private:
    static std::uint64_t Read(const char* name) {
        const char* env = std::getenv(name);
        return env ? std::strtoull(env, nullptr, 10) : 0;
    }
};

/// g2-arm64: the calling thread's thread pointer, as an identity: one register read, where a
/// thread_local in a dlopen-ed library costs a call to the dynamic TLS resolver per access.
static inline std::uintptr_t bb_thread_pointer() {
#if defined(__aarch64__)
    std::uintptr_t tp;
    asm("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
#elif defined(_WIN32)
    return static_cast<std::uintptr_t>(__readgsqword(0x30)); // the TEB
#else
    std::uintptr_t tp;
    asm("mov %%fs:0, %0" : "=r"(tp));
    return tp;
#endif
}

/// One thread's identity in a cache line of its own (read on every check, written once).
struct alignas(64) BbThreadSlot {
    std::atomic<std::uintptr_t> thread{0};

    [[nodiscard]] bool IsCurrent() const noexcept {
        return thread.load(std::memory_order_relaxed) == bb_thread_pointer();
    }
    void SetCurrent() noexcept {
        if (!IsCurrent()) {
            thread.store(bb_thread_pointer(), std::memory_order_relaxed);
        }
    }
};
