// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <thread>


#include "common/error.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "ntapi.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#include "common/string_util.h"
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#include <pthread.h>
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

#ifdef __APPLE__

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // CPU time to grant.
    const std::chrono::nanoseconds computation_ns = period_ns / 2;

    // Determine the timebase for converting time to ticks.
    struct mach_timebase_info timebase{};
    mach_timebase_info(&timebase);
    const auto ticks_per_ns =
        static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

    const auto period_ticks =
        static_cast<u32>(static_cast<double>(period_ns.count()) * ticks_per_ns);
    const auto computation_ticks =
        static_cast<u32>(static_cast<double>(computation_ns.count()) * ticks_per_ns);

    thread_time_constraint_policy policy = {
        .period = period_ticks,
        .computation = computation_ticks,
        // Should not matter since preemptible is false, but needs to be >= computation regardless.
        .constraint = computation_ticks,
        .preemptible = false,
    };

    int ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (ret != KERN_SUCCESS) {
        LOG_ERROR(Common, "Could not set thread to real-time with period {} ns: {}",
                  period_ns.count(), ret);
    }
}

#else

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // Not implemented
}

#endif

#ifdef _WIN32

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();

    LARGE_INTEGER interval{
        .QuadPart = -1 * (duration.count() / 100u),
    };
    HANDLE timer = ::CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &interval, 0, NULL, NULL, 0);
    const auto ret = WaitForSingleObjectEx(timer, INFINITE, interruptible);
    ::CloseHandle(timer);

    if (remaining) {
        const auto end_sleep = std::chrono::high_resolution_clock::now();
        const auto sleep_time = end_sleep - begin_sleep;
        *remaining = duration > sleep_time ? duration - sleep_time : std::chrono::nanoseconds(0);
    }
    return ret == WAIT_OBJECT_0;
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    pthread_t this_thread = pthread_self();

    const auto scheduling_type = SCHED_OTHER;
    s32 max_prio = sched_get_priority_max(scheduling_type);
    s32 min_prio = sched_get_priority_min(scheduling_type);
    u32 level = std::max(static_cast<u32>(new_priority) + 1, 4U);

    struct sched_param params;
    if (max_prio > min_prio) {
        params.sched_priority = min_prio + ((max_prio - min_prio) * level) / 4;
    } else {
        params.sched_priority = min_prio - ((min_prio - max_prio) * level) / 4;
    }

    pthread_setschedparam(this_thread, scheduling_type, &params);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible) {
    timespec request = {
        .tv_sec = duration.count() / 1'000'000'000,
        .tv_nsec = duration.count() % 1'000'000'000,
    };
    timespec remain;
    int ret;
    while ((ret = nanosleep(&request, &remain)) < 0 && errno == EINTR) {
        if (interruptible) {
            break;
        }
        request = remain;
    }
    if (remaining) {
        *remaining = std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0 || errno != EINTR;
}

#endif

#ifdef _WIN32

// Sets the debugger-visible name of the current thread.
void SetCurrentThreadName(const char* name) {
    // bbport: guest thread names are kept by the C runtime.
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
}

void SetThreadName(void* thread, const char* name) {
    SetThreadDescription(thread, UTF8ToUTF16W(name).data());
}

#else // !_WIN32, so must be POSIX threads

#if defined(__linux__)
namespace {
/// bbport: BB_RENDER_CPUS=<cpu list, e.g. 3-7 or 3,5-7>: the host GPU threads (command
/// processor, draw recording and preparation, Vulkan recorder, presentation, the landing of
/// deferred raycaster copies) run on these CPUs only. On the Retroid Pocket G2 the scheduler put
/// the draw recorder and the Vulkan recorder mostly on the slow A520 cores (0-2) while the
/// guest's job workers, idling in WFE spin loops that count as busy, held the A720/X4 cores.
void ApplyRenderAffinity(const std::string& name) {
    static const char* const prefixes[] = {"shadPS4:GpuCom", "bb:Draw",    "bb:VkRecorder",
                                           "bb:Present",     "bb:Landing", "shadPS4:Present"};
    bool render = false;
    for (const char* prefix : prefixes) {
        render = render || name.rfind(prefix, 0) == 0;
    }
    if (!render) {
        return;
    }
    static const cpu_set_t* const set = []() -> const cpu_set_t* {
        const char* env = std::getenv("BB_RENDER_CPUS");
        if (!env || !*env) {
            return nullptr;
        }
        static cpu_set_t cpus;
        CPU_ZERO(&cpus);
        for (const char* p = env; *p;) {
            char* end;
            const long first = std::strtol(p, &end, 10);
            long last = first;
            if (end == p) {
                break;
            }
            if (*end == '-') {
                p = end + 1;
                last = std::strtol(p, &end, 10);
            }
            for (long cpu = first; cpu <= last && cpu < CPU_SETSIZE; ++cpu) {
                CPU_SET(cpu, &cpus);
            }
            p = *end == ',' ? end + 1 : end;
            if (*end != ',' && *end) {
                break;
            }
        }
        return CPU_COUNT(&cpus) ? &cpus : nullptr;
    }();
    if (set && sched_setaffinity(0, sizeof(*set), set) == 0) {
        std::printf("Thread %s: CPUs %s\n", name.c_str(), std::getenv("BB_RENDER_CPUS"));
    }
}

/// g2-arm64: BB_HOST_NICE=<thread name prefix>=<nice>,...: the nice level of the port's own
/// threads by name (e.g. bb:VkRecorder=-10,bb:DrawRec=-6). Every thread of the process runs at
/// the guest's display priority (nice -4) otherwise, and the stage that submits to the GPU
/// waits in the run queue behind the game's job workers (12% of its time in U3's pub).
void ApplyHostNice(const std::string& name) {
    static const char* const env = std::getenv("BB_HOST_NICE");
    if (!env || !*env) {
        return;
    }
    for (const char* p = env; *p;) {
        const char* eq = std::strchr(p, '=');
        if (!eq) {
            break;
        }
        const char* end = std::strchr(eq, ',');
        const std::string prefix(p, eq);
        const long nice = std::strtol(eq + 1, nullptr, 10);
        if (!prefix.empty() && name.rfind(prefix, 0) == 0) {
            if (setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), static_cast<int>(nice)) == 0) {
                std::printf("Thread %s: nice %ld\n", name.c_str(), nice);
            } else {
                std::printf("Thread %s: nice %ld refused: %s\n", name.c_str(), nice,
                            GetLastErrorMsg().c_str());
            }
            return;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
}
} // namespace
#endif

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
    // bbport: guest thread names are kept by the C runtime.
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
    ApplyRenderAffinity(truncated);
    ApplyHostNice(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadName(void* thread, const char* name) {
    // TODO
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char* name) {
    // bbport: guest thread names are kept by the C runtime.
    // Do Nothing on MinGW
}

void SetThreadName(void* thread, const char* name) {
    // Do Nothing on MinGW
}
#endif

#endif

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval)
    : target_interval(target_interval) {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::high_resolution_clock::now();
    if (total_wait.count() > 0) {
        AccurateSleep(total_wait, nullptr, false);
    }
    start_time = std::chrono::high_resolution_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    auto now = std::chrono::high_resolution_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);
}

std::string GetCurrentThreadName() {
#ifdef _WIN32
    PWSTR name{};
    if (FAILED(GetThreadDescription(GetCurrentThread(), &name)) || name == nullptr) {
        return "<unknown name>";
    }
    const auto result = Common::UTF16ToUTF8(name);
    LocalFree(name);
    return result;
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
#endif
}

} // namespace Common
