// SPDX-FileCopyrightText: Copyright 2026 uncharted-android
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// g2-arm64 diagnosis: BB_SUBMIT_STATS=1 prints every 5 s where the Vulkan submissions' time
// goes: per call site and thread, the steps of Scheduler::SubmitExecution (the submit lock, the
// submit callback's parts, the recording sync, the host copies, vkQueueSubmit, ...), of the
// recording thread's deferred submissions and of the presenter (acquire, flush, the submit lock,
// vkQueuePresentKHR); how long each site held Scheduler::submit_mutex and which holder the waits
// of 1 ms or more met; the draw scheduler's submissions in flight (submitted, not yet done) when
// one is submitted. The question: the recording thread's mid-frame submits took 7.8 ms each,
// ~25 a second (about one a frame), in U3's museum (findings 2026-10-31), while vkQueueSubmit
// itself took 31-50 us and the recording syncs 0.4-1 ms on average.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#ifndef _WIN32
#include <pthread.h>
#endif

namespace BbSubmitStats {

using Clock = std::chrono::steady_clock;
constexpr int MaxSteps = 14;
constexpr std::uint64_t SlowNs = 1000000; // 1 ms

inline bool On() {
    static const bool on = [] {
        const char* env = std::getenv("BB_SUBMIT_STATS");
        return env && env[0] == '1';
    }();
    return on;
}

inline std::uint64_t Ns(Clock::duration d) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

inline const char* ThreadName() {
    thread_local char name[24] = "";
    if (!name[0]) {
#ifndef _WIN32
        if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0 || !name[0])
#endif
            std::snprintf(name, sizeof(name), "?");
    }
    return name;
}

struct Site {
    const char* site = nullptr;
    char thread[24] = "";
    const char* step_names[MaxSteps]{};
    int num_steps = 0;
    std::uint64_t count = 0, total_ns = 0, max_ns = 0;
    std::uint64_t step_ns[MaxSteps]{};
    std::uint64_t slow = 0, slow_ns = 0; // calls of 1 ms or more
    std::uint64_t slow_step_ns[MaxSteps]{};
};

struct Hold {
    const char* site = nullptr;
    std::uint64_t count = 0, total_ns = 0, max_ns = 0;
    std::uint64_t waited_on = 0, waited_on_ns = 0; // waits of 1 ms or more that met this holder
};

struct State {
    std::mutex mutex;
    Clock::time_point window_start{};
    std::array<Site, 24> sites{};
    std::array<Hold, 8> holds{};
    std::uint64_t dropped = 0;
    std::uint64_t unknown_waits = 0, unknown_waits_ns = 0;
    std::uint64_t inflight_samples = 0, inflight_sum = 0, inflight_max = 0;
    std::array<std::uint64_t, 6> inflight_hist{}; // 0, 1, 2, 3, 4, 5 or more
};

inline State& Get() {
    static State state;
    return state;
}

/// The site holding Scheduler::submit_mutex (set while held, with BB_SUBMIT_STATS).
inline std::atomic<const char*> holder{nullptr};

inline Hold& HoldFor(State& s, const char* site) {
    for (auto& h : s.holds) {
        if (h.site == site) {
            return h;
        }
        if (!h.site) {
            h.site = site;
            return h;
        }
    }
    return s.holds.back();
}

inline void Append(std::string& out, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
inline void Append(std::string& out, const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    out += buf;
}

/// With the stats mutex held.
inline void MaybePrint(State& s, Clock::time_point now) {
    if (s.window_start.time_since_epoch().count() == 0) {
        s.window_start = now;
        return;
    }
    if (now - s.window_start < std::chrono::seconds(5)) {
        return;
    }
    const double secs = std::chrono::duration<double>(now - s.window_start).count();
    std::string out;
    Append(out, "Submit stats over %.1f s:\n", secs);
    for (auto& site : s.sites) {
        if (!site.site || !site.count) {
            continue;
        }
        Append(out, "  %s on %s: %.1f/s, %.0f us each (max %.0f); us each:", site.site,
               site.thread, site.count / secs, site.total_ns / 1e3 / site.count,
               site.max_ns / 1e3);
        for (int i = 0; i < site.num_steps; ++i) {
            Append(out, "%s %s %.0f", i ? "," : "", site.step_names[i],
                   site.step_ns[i] / 1e3 / site.count);
        }
        if (site.slow) {
            Append(out, "; 1 ms or more: %.1f/s, %.0f us each:", site.slow / secs,
                   site.slow_ns / 1e3 / site.slow);
            for (int i = 0; i < site.num_steps; ++i) {
                Append(out, "%s %s %.0f", i ? "," : "", site.step_names[i],
                       site.slow_step_ns[i] / 1e3 / site.slow);
            }
        }
        out += '\n';
        site.count = site.total_ns = site.max_ns = site.slow = site.slow_ns = 0;
        std::fill(std::begin(site.step_ns), std::end(site.step_ns), 0);
        std::fill(std::begin(site.slow_step_ns), std::end(site.slow_step_ns), 0);
    }
    out += "  submit lock held by:";
    for (auto& h : s.holds) {
        if (h.site && h.count) {
            Append(out, " %s %.1f/s %.0f us (max %.0f);", h.site, h.count / secs,
                   h.total_ns / 1e3 / h.count, h.max_ns / 1e3);
        }
    }
    out += "\n  submit lock waits of 1 ms or more met:";
    for (auto& h : s.holds) {
        if (h.site && h.waited_on) {
            Append(out, " %s %.1f/s %.0f us each;", h.site, h.waited_on / secs,
                   h.waited_on_ns / 1e3 / h.waited_on);
        }
        h.count = h.total_ns = h.max_ns = h.waited_on = h.waited_on_ns = 0;
    }
    if (s.unknown_waits) {
        Append(out, " (holder not seen) %.1f/s %.0f us each;", s.unknown_waits / secs,
               s.unknown_waits_ns / 1e3 / s.unknown_waits);
    }
    out += '\n';
    if (s.inflight_samples) {
        Append(out, "  draw submissions in flight when one is submitted: %.2f, max %llu;",
               double(s.inflight_sum) / s.inflight_samples,
               static_cast<unsigned long long>(s.inflight_max));
        for (std::size_t i = 0; i < s.inflight_hist.size(); ++i) {
            Append(out, " %zu%s %.0f%%", i, i + 1 == s.inflight_hist.size() ? "+" : "",
                   100.0 * s.inflight_hist[i] / s.inflight_samples);
        }
        out += '\n';
    }
    if (s.dropped) {
        Append(out, "  (%llu samples with other steps dropped)\n",
               static_cast<unsigned long long>(s.dropped));
    }
    s.unknown_waits = s.unknown_waits_ns = s.dropped = 0;
    s.inflight_samples = s.inflight_sum = s.inflight_max = 0;
    s.inflight_hist.fill(0);
    s.window_start = now;
    std::fputs(out.c_str(), stdout);
}

/// Times the steps of one call: Step(name) closes the step since the previous one. The
/// innermost Steps of a thread is current: StepHere() from code it calls adds steps to it.
class Steps;
inline thread_local Steps* current = nullptr;

class Steps {
public:
    explicit Steps(const char* site) : site_(site), on_(On()) {
        if (on_) {
            start_ = last_ = Clock::now();
            previous_ = current;
            current = this;
        }
    }
    Steps(const Steps&) = delete;
    Steps& operator=(const Steps&) = delete;
    ~Steps() {
        if (on_) {
            current = previous_;
            Finish();
        }
    }
    void SetSite(const char* site) {
        site_ = site;
    }
    void Step(const char* name) {
        if (!on_ || n_ >= MaxSteps) {
            return;
        }
        const auto now = Clock::now();
        names_[n_] = name;
        ns_[n_] = Ns(now - last_);
        ++n_;
        last_ = now;
    }
    /// A value that is not a step of its own (a part of the step before), in ns.
    void Value(const char* name, std::uint64_t ns) {
        if (!on_ || n_ >= MaxSteps) {
            return;
        }
        names_[n_] = name;
        ns_[n_] = ns;
        ++n_;
    }
    /// Drops this call (a path with other steps).
    void Cancel() {
        if (on_) {
            current = previous_;
            on_ = false;
        }
    }

private:
    void Finish() {
        const auto now = Clock::now();
        const std::uint64_t total = Ns(now - start_);
        const char* thread = ThreadName();
        State& s = Get();
        std::scoped_lock lk{s.mutex};
        Site* site = nullptr;
        for (auto& candidate : s.sites) {
            if (candidate.site == site_ && std::strcmp(candidate.thread, thread) == 0) {
                site = &candidate;
                break;
            }
            if (!candidate.site) {
                candidate.site = site_;
                std::snprintf(candidate.thread, sizeof(candidate.thread), "%s", thread);
                candidate.num_steps = n_;
                std::copy_n(names_, n_, candidate.step_names);
                site = &candidate;
                break;
            }
        }
        if (!site || site->num_steps != n_) {
            ++s.dropped;
            MaybePrint(s, now);
            return;
        }
        ++site->count;
        site->total_ns += total;
        site->max_ns = std::max(site->max_ns, total);
        for (int i = 0; i < n_; ++i) {
            site->step_ns[i] += ns_[i];
        }
        if (total >= SlowNs) {
            ++site->slow;
            site->slow_ns += total;
            for (int i = 0; i < n_; ++i) {
                site->slow_step_ns[i] += ns_[i];
            }
        }
        MaybePrint(s, now);
    }

    const char* site_;
    bool on_;
    int n_ = 0;
    Clock::time_point start_{}, last_{};
    Steps* previous_ = nullptr;
    const char* names_[MaxSteps]{};
    std::uint64_t ns_[MaxSteps]{};
};

/// The calling thread's time on a CPU and runnable in a run queue so far (ns), from
/// /proc/thread-self/schedstat: a long step split into running, preempted and asleep.
inline bool SchedStat(std::uint64_t& run_ns, std::uint64_t& queue_ns) {
#ifndef _WIN32
    FILE* f = std::fopen("/proc/thread-self/schedstat", "r");
    if (!f) {
        return false;
    }
    unsigned long long run = 0, queue = 0;
    const int read = std::fscanf(f, "%llu %llu", &run, &queue);
    std::fclose(f);
    if (read != 2) {
        return false;
    }
    run_ns = run;
    queue_ns = queue;
    return true;
#else
    return false;
#endif
}

inline void StepHere(const char* name) {
    if (current) {
        current->Step(name);
    }
}

inline void NoteInflight(std::uint64_t n) {
    State& s = Get();
    std::scoped_lock lk{s.mutex};
    ++s.inflight_samples;
    s.inflight_sum += n;
    s.inflight_max = std::max(s.inflight_max, n);
    ++s.inflight_hist[std::min<std::uint64_t>(n, s.inflight_hist.size() - 1)];
}

/// Locks Scheduler::submit_mutex for a site: the hold time per site, the waits of 1 ms or more
/// by the holder they met.
class TimedLock {
public:
    TimedLock(std::mutex& mutex, const char* site) : mutex_(mutex), site_(site), on_(On()) {
        if (!on_) {
            mutex_.lock();
            return;
        }
        if (!mutex_.try_lock()) {
            const char* seen = holder.load(std::memory_order_relaxed);
            const auto begin = Clock::now();
            mutex_.lock();
            const std::uint64_t waited = Ns(Clock::now() - begin);
            if (waited >= SlowNs) {
                State& s = Get();
                std::scoped_lock lk{s.mutex};
                if (seen) {
                    Hold& h = HoldFor(s, seen);
                    ++h.waited_on;
                    h.waited_on_ns += waited;
                } else {
                    ++s.unknown_waits;
                    s.unknown_waits_ns += waited;
                }
            }
        }
        holder.store(site_, std::memory_order_relaxed);
        acquired_ = Clock::now();
    }
    TimedLock(const TimedLock&) = delete;
    TimedLock& operator=(const TimedLock&) = delete;
    ~TimedLock() {
        if (!on_) {
            mutex_.unlock();
            return;
        }
        const std::uint64_t held = Ns(Clock::now() - acquired_);
        holder.store(nullptr, std::memory_order_relaxed);
        mutex_.unlock();
        State& s = Get();
        std::scoped_lock lk{s.mutex};
        Hold& h = HoldFor(s, site_);
        ++h.count;
        h.total_ns += held;
        h.max_ns = std::max(h.max_ns, held);
    }

private:
    std::mutex& mutex_;
    const char* site_;
    bool on_;
    Clock::time_point acquired_{};
};

} // namespace BbSubmitStats
