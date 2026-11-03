// SPDX-FileCopyrightText: Copyright 2026 uncharted-android
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// g2-arm64: BB_PRESENT_PACING - frames under a frame limit below the display's rate go to the
// screen for the same number of refreshes each. The present thread's limiter counts its slots on
// the CPU clock, blind to the display's vsyncs: at 30 FPS on the G2's 60 Hz panel 13% of the
// frames stayed on the screen for 1 or 3 refreshes instead of 2 (SurfaceFlinger's present times,
// findings 2026-11-01), nearly all in pairs - a frame late for SurfaceFlinger's latch, the next
// one on time - the uneven frame times the owner saw. Paced, the slots are counted from the
// vsyncs SurfaceFlinger reports (VK_GOOGLE_display_timing, which Android's libvulkan implements
// over the window's frame timestamps), each request taken a lead before its vsync, and every
// frame carries a desired present time half a refresh before that vsync: SurfaceFlinger shows it
// on that vsync and never earlier. A frame whose GPU work ends after SurfaceFlinger's latch for
// that vsync (~16 ms before it on the G2) still goes out a refresh late; in U1's statue garden
// the work ended 2-26 ms after the request was taken (median 2, p99 26), so the lead adapts:
// up while frames come late, slowly down while none do (30 ms to start, at most 50). The present
// thread moves its slots toward a new lead by at most 1 ms a frame: a jump of 8-16 ms at once
// moved one frame's slot as much, a visible hitch (the owner's play, 2026-11-01).
//   unset, 1  paced, with the display cadence statistics (on by default since the interleaved
//             A/B of 2026-11-01: U1 statue garden at the 30 FPS limit, 4 pairs, 2.0% of the
//             frames off the cadence against 14.9%, the frame rate within 0.1);
//   0         off: no present times (0.2.11's presenting);
//   2         the presents carry IDs: the statistics only.
// BB_PRESENT_PACING_LEAD_US (30000): the lead to start with. It adapts by the presents' margins
// (BB_PRESENT_PACING_ADAPT unset or 2, the default since 2026-11-03): 2 ms more at every frame
// shown late, 1 ms less only after 10 s without one in which every frame reached SurfaceFlinger's
// latch 8 ms early, or, without margins from the driver, after 20 s. U1 at the 30 FPS limit, 4
// interleaved pairs of 90 s (findings 2026-11-03 "U3's workers asleep"): frames on the screen for
// 3+ refreshes 18-43 a run -> 6-11, off the cadence 1.4-2.2% -> 0.3-0.4%, the lead near its cap,
// ~10 ms more from the guest's flip to the screen (100-106 -> 112-113 ms). =1: the rule before,
// which probes down 1 ms every 2 s and so keeps finding late frames, each a hold and a jump on the
// screen (findings 2026-11-02 "frames go missing"), which the launcher keeps for U2 (its frames
// reach the present queue late, and at the margin rule's cap more of them missed their take: 4.6-6.1%
// of them held 3 refreshes against 0.5-3.6%; U3 0.8-0.9% -> 0.3% with the margin rule, findings
// 2026-11-03); =0: the lead fixed;
// BB_PRESENT_PACING_MAX_US (50000): the most;
// BB_PRESENT_PACING_MIN_US (22000): a request taken late goes to a vsync at least this far away
// and at least the lead less a refresh (a game behind is usually GPU-bound: its frame's work ends
// late too); the lead stays 2 ms above it.
// BB_PRESENT_TRACE=1 (diagnosis): a `Frame trace` line per paced frame shown (see NoteShown).
// The `Display cadence` line counts the presents whose timing never came back (not shown: replaced
// before SurfaceFlinger took them, or the timing lost).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace BbPresentPacing {

inline int Mode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_PRESENT_PACING");
        return !env ? 1 : env[0] == '0' ? 0 : env[0] == '2' ? 2 : 1;
    }();
    return mode;
}

inline int64_t EnvNs(const char* name, long fallback_us) {
    const char* env = std::getenv(name);
    return int64_t(env ? std::strtol(env, nullptr, 10) : fallback_us) * 1000;
}

inline int64_t LeadStart() {
    static const int64_t lead = EnvNs("BB_PRESENT_PACING_LEAD_US", 30000);
    return lead;
}

inline int64_t LeadMax() {
    static const int64_t lead = EnvNs("BB_PRESENT_PACING_MAX_US", 50000);
    return lead;
}

inline int64_t LeadMin() {
    static const int64_t lead = EnvNs("BB_PRESENT_PACING_MIN_US", 22000);
    return lead;
}

/// 0: the lead fixed; 1: the rule before 2026-11-03 (every 60 paced frames: 2 ms more per late
/// frame from 2 on, 1 ms less with none); 2, the default: up at every late frame, down only with
/// margin to spare.
inline int AdaptMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_PRESENT_PACING_ADAPT");
        return !env ? 2 : env[0] == '0' ? 0 : env[0] == '1' ? 1 : 2;
    }();
    return mode;
}

inline bool Adapt() {
    return AdaptMode() != 0;
}

inline bool Trace() {
    static const bool trace = [] {
        const char* env = std::getenv("BB_PRESENT_TRACE");
        return env && env[0] == '1';
    }();
    return trace;
}

/// BB_PRESENT_TRACE: the guest's flips (gnmdriver, the guest's thread): its flip argument, when it
/// called, how long it waited for the submission lock and the GPU side's idle; found again by the
/// argument when the flip reaches the present thread's queue (the GPU thread).
struct GuestFlip {
    int64_t arg = 0, guest_ns = 0, waited_ns = 0;
};
inline std::mutex guest_flips_mutex;
inline GuestFlip guest_flips[16];
inline uint32_t guest_flips_next = 0;

inline void NoteGuestFlip(int64_t arg, int64_t guest_ns, int64_t waited_ns) {
    std::scoped_lock lock{guest_flips_mutex};
    guest_flips[guest_flips_next++ % 16] = {arg, guest_ns, waited_ns};
}

inline GuestFlip FindGuestFlip(int64_t arg) {
    std::scoped_lock lock{guest_flips_mutex};
    for (uint32_t i = 0; i < 16; ++i) {
        const GuestFlip& flip = guest_flips[(guest_flips_next - 1 - i) % 16];
        if (flip.guest_ns && flip.arg == arg) {
            return flip;
        }
    }
    return {};
}

/// BB_PRESENT_TRACE: a paced frame's times (CLOCK_MONOTONIC ns): the guest's flip, its wait, the
/// request queued, taken, the slot it was due at (the present thread), handed to
/// vkQueuePresentKHR (the presenter's thread); kept by present ID until it is shown.
enum TraceTime { kGuest, kWaited, kQueued, kTaken, kDue, kTraceTimes };
struct FrameTrace {
    uint32_t id = 0;
    int64_t arg = 0;
    int64_t t[kTraceTimes] = {};
    int64_t presented = 0;
};
inline FrameTrace frame_traces[64];

inline void NotePresented(uint32_t id, int64_t arg, const int64_t* t, int64_t presented) {
    FrameTrace& trace = frame_traces[id % 64];
    trace.id = id;
    trace.arg = arg;
    std::copy(t, t + kTraceTimes, trace.t);
    trace.presented = presented;
}

/// The lead wanted (the presenter's thread adapts it, the present thread steps toward it); 0 = the
/// start.
inline std::atomic<int64_t> lead_ns{0};

inline int64_t Lead() {
    const int64_t lead = lead_ns.load(std::memory_order_relaxed);
    return lead ? lead : LeadStart();
}

/// From the swapchain (the presenter's thread): the display's refresh period and the latest vsync
/// a frame was shown on, in CLOCK_MONOTONIC ns (std::chrono::steady_clock's clock on Android);
/// 0 while unknown.
inline std::atomic<int64_t> refresh_ns{0};
inline std::atomic<int64_t> vsync_ns{0};

inline int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The present thread's side: the vsync each frame is meant for.
class Pacer {
public:
    /// Refreshes a frame under a limit (frames per second); 0 when not paced: the switch, no
    /// vsync reported yet, or a limit that is not a whole number of at least 2 refreshes (30 at
    /// 60 Hz is 2; 60, 40 or none are not paced).
    int64_t Refreshes(uint32_t limit) const {
        const int64_t refresh = refresh_ns.load(std::memory_order_relaxed);
        if (Mode() != 1 || !limit || !refresh || !vsync_ns.load(std::memory_order_relaxed)) {
            return 0;
        }
        const double n = 1e9 / (double(limit) * double(refresh));
        const int64_t whole = int64_t(n + 0.5);
        return whole >= 2 && n - double(whole) < 0.05 && double(whole) - n < 0.05 ? whole : 0;
    }

    /// A request taken at `now` under `refreshes` a frame: returns its desired present time and
    /// sets `slot`, the time the next request may be taken.
    int64_t Take(int64_t now, int64_t refreshes, int64_t& slot) {
        const int64_t refresh = refresh_ns.load(std::memory_order_relaxed);
        const int64_t anchor = vsync_ns.load(std::memory_order_relaxed);
        // The grid of vsyncs through the latest reported one (it moves with every report).
        const auto floor_div = [](int64_t a, int64_t b) {
            return a >= 0 ? a / b : -((-a + b - 1) / b);
        };
        const auto at_or_after = [&](int64_t t) {
            return anchor + (floor_div(t - anchor - 1, refresh) + 1) * refresh;
        };
        const auto nearest = [&](int64_t t) {
            return anchor + floor_div(t - anchor + refresh / 2, refresh) * refresh;
        };
        // The lead in use steps toward the wanted one by at most 1 ms a frame.
        const int64_t wanted = Lead();
        lead = lead ? lead + std::clamp<int64_t>(wanted - lead, -1'000'000, 1'000'000) : wanted;
        // The earliest vsync a frame taken now can make: presented and latched in time. A request
        // taken late still makes its planned vsync within a refresh of slack.
        const int64_t earliest = at_or_after(now + std::max(LeadMin(), lead - refresh));
        const int64_t vsync = next_vsync ? std::max(nearest(next_vsync), earliest) : earliest;
        next_vsync = vsync + refreshes * refresh;
        slot = next_vsync - lead;
        return vsync - refresh / 2;
    }

    void Reset() {
        next_vsync = 0;
    }

private:
    int64_t next_vsync = 0;
    int64_t lead = 0; ///< the lead in use (0 until the first paced frame)
};

/// The presenter's side, after each present: a frame SurfaceFlinger has shown (its present ID,
/// the desired present time it carried or 0, the vsync it went out on, how long before
/// SurfaceFlinger's latch its rendering was done - 0 when the driver does not say). Counts the refreshes
/// between consecutive frames on the screen, the paced frames shown later than meant and the
/// presents never shown (gaps in the IDs), a line every 5 seconds; adapts the lead every 60 paced
/// frames: 2 ms more per late frame from 2 on (at most 10), 1 ms less when none was late. With
/// BB_PRESENT_TRACE a `Frame trace` line per paced frame: its times in CLOCK_MONOTONIC ms.
inline void NoteShown(uint32_t id, int64_t desired, int64_t actual, int64_t margin = 0) {
    static uint32_t last_id = 0;
    static int64_t last_actual = 0, window_start = 0;
    static uint32_t refreshes[5], frames, paced, late, not_shown;
    static uint32_t adapt_paced, adapt_late;
    static uint32_t quiet;                    // BB_PRESENT_PACING_ADAPT=2: paced frames since a late one
    static int64_t quiet_margin = INT64_MAX;  // the least margin among them
    static int64_t window_margin = INT64_MAX; // the least margin in the cadence line's window
    static bool margins_seen;
    const int64_t refresh = refresh_ns.load(std::memory_order_relaxed);
    if (actual <= 0 || !refresh) {
        return;
    }
    vsync_ns.store(actual, std::memory_order_relaxed);
    int64_t n = 0;
    if (last_actual && id == last_id + 1 && actual > last_actual) {
        n = (actual - last_actual + refresh / 2) / refresh;
        ++refreshes[std::clamp<int64_t>(n, 0, 4)];
        ++frames;
    } else if (last_id && id > last_id + 1 && id - last_id < 1000) {
        not_shown += id - last_id - 1;
    }
    if (Trace() && desired) {
        const FrameTrace& trace = frame_traces[id % 64];
        if (trace.id == id && trace.t[kTaken]) {
            const auto ms = [](int64_t ns) { return ns / 1e6; };
            std::printf("Frame trace: id %u, arg %lld, guest %.3f waited %.3f, queued %.3f, taken "
                        "%.3f due %.3f, presented %.3f, desired %.3f, shown %.3f, refreshes %lld, "
                        "gap %u, margin %.3f\n",
                        id, (long long)trace.arg, ms(trace.t[kGuest]), ms(trace.t[kWaited]),
                        ms(trace.t[kQueued]), ms(trace.t[kTaken]), ms(trace.t[kDue]),
                        ms(trace.presented), ms(desired), ms(actual), (long long)n,
                        last_id && id > last_id ? id - last_id - 1 : 0u, ms(margin));
            std::fflush(stdout);
        }
    }
    if (desired) {
        ++paced;
        ++adapt_paced;
        const bool shown_late = actual - (desired + refresh / 2) > refresh / 2;
        if (shown_late) {
            ++late;
            ++adapt_late;
        }
        if (margin > 0) {
            margins_seen = true;
            window_margin = std::min(window_margin, margin);
        }
        if (AdaptMode() == 2) {
            int64_t lead = Lead();
            if (shown_late) {
                lead += 2'000'000;
                quiet = 0;
                quiet_margin = INT64_MAX;
            } else {
                ++quiet;
                quiet_margin = std::min(quiet_margin, margin);
                const bool spare = margins_seen ? quiet >= 300 && quiet_margin >= 8'000'000
                                                : quiet >= 600;
                if (spare) {
                    lead -= 1'000'000;
                }
                if (spare || (margins_seen && quiet >= 300)) {
                    quiet = 0;
                    quiet_margin = INT64_MAX;
                }
            }
            const int64_t least = LeadMin() + 2'000'000;
            lead_ns.store(std::clamp(lead, least, std::max(LeadMax(), least)),
                          std::memory_order_relaxed);
            adapt_paced = adapt_late = 0;
        } else if (adapt_paced >= 60) {
            int64_t lead = Lead();
            if (adapt_late >= 2) {
                lead += std::min<int64_t>(adapt_late, 5) * 2'000'000;
            } else if (adapt_late == 0) {
                lead -= 1'000'000;
            }
            if (Adapt()) {
                const int64_t least = LeadMin() + 2'000'000;
                lead_ns.store(std::clamp(lead, least, std::max(LeadMax(), least)),
                              std::memory_order_relaxed);
            }
            adapt_paced = adapt_late = 0;
        }
    }
    last_id = id;
    last_actual = actual;
    if (!window_start) {
        window_start = actual;
    } else if (actual - window_start >= 5'000'000'000) {
        const auto share = [&](uint32_t n) { return frames ? 100.0 * n / frames : 0.0; };
        std::printf("Display cadence over %.1f s: %u frames; refreshes each 1: %.1f%%, 2: %.1f%%, "
                    "3: %.1f%%, 4+: %.1f%%; paced %u, %u of them late; lead %.1f ms; %u not "
                    "shown; margin %.1f ms at least\n",
                    (actual - window_start) / 1e9, frames, share(refreshes[1]), share(refreshes[2]),
                    share(refreshes[3]), share(refreshes[4]), paced, late, Lead() / 1e6, not_shown,
                    window_margin == INT64_MAX ? 0.0 : window_margin / 1e6);
        std::fflush(stdout);
        window_start = actual;
        std::fill(std::begin(refreshes), std::end(refreshes), 0u);
        frames = paced = late = not_shown = 0;
        window_margin = INT64_MAX;
    }
}

} // namespace BbPresentPacing
