// SPDX-License-Identifier: GPL-2.0-or-later
// g2-arm64: guest memory faults on the GPU side's helper threads (the Vulkan recording thread,
// the copy threads).
//
// The helpers write guest memory for the GPU side: the fence labels the draw recording thread
// hands over (RecorderFences). A write to a page holding GPU data is resolved where the caches
// are owned: on the GPU command thread (stage A) with the draw pipe empty, or on the draw
// recording thread (stage B) inside a packet. A helper sent it to the command thread as guest
// threads do (SendCommand<true>) and deadlocked when the owner was itself waiting for that
// helper. U3's first chapter: stage B in SyncRecording waited for the recording thread, the
// recording thread (a fence label) for stage A, and stage A in DrawPipe::Drain for stage B.
// Now the owner announces such waits (Scope), and a helper whose fault needs the owner runs
// the resolution itself while one lasts: the owner cannot leave that wait before it is done.
// Otherwise the command thread runs it, as before.
#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace BbOwnerWait {

/// Set on the helper threads (read only when a fault needs the owner).
inline thread_local bool helper_thread = false;
/// Whether the calling thread owns the caches (set by the rasterizer).
inline bool (*owns_caches)() = nullptr;

/// Held while a helper runs owner work, and to change `waiting`.
inline std::mutex mutex;
/// Owners inside a Scope (guarded by mutex).
inline int waiting = 0;
/// Helpers waiting for owner work: an owner entering a Scope wakes them.
inline std::atomic<int> helpers_waiting{0};
/// Bumped when an owner enters a Scope while helpers wait, and when owner work is done.
inline std::atomic<unsigned> events{0};
/// Owner work a helper ran itself (diagnostics).
inline std::atomic<unsigned> helper_runs{0};

inline void Wake() {
    events.fetch_add(1, std::memory_order_seq_cst);
    events.notify_all();
}

/// Around a wait of the cache owner for the helpers (directly, or for a submission deferred to
/// the recording thread). Does nothing on other threads.
class Scope {
public:
    Scope() : owner{owns_caches && owns_caches()} {
        if (!owner) {
            return;
        }
        {
            std::scoped_lock lk{mutex};
            ++waiting;
        }
        if (helpers_waiting.load(std::memory_order_seq_cst) > 0) {
            Wake();
        }
    }
    ~Scope() {
        if (owner) {
            std::scoped_lock lk{mutex}; // after the owner work a helper is running
            --waiting;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool owner;
};

/// Owner work from a helper thread. `send` queues a command for the GPU command thread (which
/// runs commands with the draw pipe empty); if an owner waits in a Scope first, the helper runs
/// `work` itself. Returns once `work` has run.
template <typename Work, typename Send>
void RunFromHelper(Work&& work, Send&& send) {
    struct Request {
        explicit Request(Work&& w) : work{std::forward<Work>(w)} {}
        std::decay_t<Work> work;
        std::atomic<int> state{0}; // 0 queued, 1 running, 2 done
        bool Claim() {
            int expected = 0;
            return state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
        }
        void Run() {
            work();
            state.store(2, std::memory_order_release);
            Wake();
        }
    };
    const auto request = std::make_shared<Request>(std::forward<Work>(work));
    send([request] {
        // The command thread runs commands with the draw pipe empty: no other owner is in a
        // Scope, so no helper runs owner work meanwhile. Not under `mutex`: work that waits for
        // the GPU opens a Scope itself (Scheduler::Wait), which takes it - the thread would wait
        // for itself.
        if (request->Claim()) {
            request->Run();
        }
    });
    helpers_waiting.fetch_add(1, std::memory_order_seq_cst);
    while (request->state.load(std::memory_order_acquire) != 2) {
        const unsigned seen = events.load(std::memory_order_seq_cst);
        bool ran = false;
        {
            std::scoped_lock lk{mutex};
            if (waiting > 0 && request->Claim()) {
                request->Run();
                ran = true;
            }
        }
        if (ran) {
            if (const unsigned runs = helper_runs.fetch_add(1, std::memory_order_relaxed);
                runs < 10) {
                std::printf("GPU: a helper thread resolved its fault while the owner waited for "
                            "it (%u)\n",
                            runs + 1);
            }
            break;
        }
        if (request->state.load(std::memory_order_acquire) == 2) {
            break;
        }
        events.wait(seen, std::memory_order_seq_cst);
    }
    helpers_waiting.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace BbOwnerWait
