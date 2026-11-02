// SPDX-FileCopyrightText: Copyright 2026 uncharted-android
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// g2-arm64: BB_PRESENT_UNLOCK (on; =0 off) - the presenter holds Scheduler::submit_mutex around
// vkQueuePresentKHR (a queue is externally synchronized), but on Android only the driver's
// QueueSignalReleaseImageANDROID inside it uses the queue; the window's queueBuffer that follows
// (libvulkan, swapchain.cpp) does not, and it slept 3-9 ms a frame in U3's museum while every
// submission waited (BB_SUBMIT_STATS, findings 2026-11-01). The window's queueBuffer is wrapped
// (vk_platform.cpp) and lets the lock go for its duration.

#include <cstdlib>
#include <mutex>

namespace BbPresentUnlock {

inline bool Wanted() {
    // On by default since the interleaved A/Bs of 2026-11-01 (U3 museum +3.0 FPS, 4 pairs of 4; U1 statue garden +0.1-0.4, no loss); =0 turns it off.
    static const bool wanted = [] {
        const char* env = std::getenv("BB_PRESENT_UNLOCK");
        return !(env && env[0] == '0');
    }();
    return wanted;
}

/// Set by the presenter around vkQueuePresentKHR on its own thread: the lock it holds there.
inline thread_local std::mutex* queue_lock = nullptr;

/// True once a window's queueBuffer is wrapped (the table matched the expected layout).
inline bool hooked = false;

} // namespace BbPresentUnlock
