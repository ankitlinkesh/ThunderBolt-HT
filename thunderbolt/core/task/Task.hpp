// Thunderbolt HT - the internal task record.
//
// Lives under core/, not api/: application code names TaskHandle and TaskDesc,
// never this (S66).
#pragma once

#include <thunderbolt/api/TaskFunction.hpp>
#include <thunderbolt/api/TaskHandle.hpp>
#include <thunderbolt/api/TaskTypes.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

namespace thunderbolt {

// One pooled task slot.
//
// Cache-line aligned because `state` is written by whichever worker executes the
// task while neighbouring slots are being written by other workers. Without the
// alignment, tasks that share a line would ping-pong that line between cores on
// every state transition - a slowdown that looks exactly like poor scheduling
// while actually being layout.
TB_BEGIN_CACHE_ALIGNED_TYPE
struct alignas(kCacheLineSize) Task {
    // Successors that fit without allocating. A frame graph node typically feeds
    // a handful of dependents; beyond this the list spills to the heap, and the
    // spill is counted so Phase E can say whether it ever mattered.
    static constexpr std::size_t kInlineSuccessors = 6;

    TaskFunction function;

    // Generation of the handle currently occupying this slot. Incremented on
    // release, which is what makes a handle to a recycled slot detectably stale
    // rather than silently pointing at someone else's task (S62).
    std::atomic<std::uint32_t> generation{0};

    // S7 lifecycle position.
    std::atomic<TaskState> state{TaskState::Free};

    // Dependencies not yet satisfied. The task becomes runnable when this hits
    // zero. Submission adds an extra +1 guard while the dependency list is still
    // being built, so a predecessor completing mid-registration cannot enqueue a
    // task whose remaining edges have not been counted yet.
    std::atomic<std::uint32_t> pending_dependencies{0};

    TaskPriority  priority       = TaskPriority::Normal;
    TaskFlags     flags          = TaskFlags::None;
    std::uint32_t estimated_cost = 0;

    Task()                       = default;
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    // --- successor list --------------------------------------------------
    //
    // Guarded by a spin flag rather than a mutex: it is held for the duration of
    // a single push or a single hand-off, and a std::mutex per task would cost
    // more in size and construction than the contention it avoids.

    // Registers `successor` to be notified when this task completes.
    //
    // Returns false when the registration did NOT happen, which means this task
    // has already completed (or its slot was recycled) and the caller must
    // account for the dependency as already satisfied. `expected_generation` is
    // re-checked under the lock: without that, a slot released and re-acquired
    // between the caller's lookup and this call would silently attach the
    // successor to an unrelated task, and it would then wait for the wrong one.
    bool try_add_successor(TaskHandle successor, std::uint32_t expected_generation) {
        if (fast_successors_) {
            return fast_add(successor, expected_generation);
        }
        lock();
        const bool usable =
            !successors_closed_ && generation.load(std::memory_order_relaxed) == expected_generation;
        if (usable) {
            if (successor_count_ < kInlineSuccessors) {
                inline_successors_[successor_count_] = successor;
            } else {
                spilled_successors_.push_back(successor);
            }
            ++successor_count_;
        }
        unlock();
        return usable;
    }

    // Closes the list and hands its contents to `out`. Called exactly once, by
    // the thread completing this task, BEFORE the pool slot is released - so any
    // registration that got in first is guaranteed to be seen here.
    void take_successors(std::vector<TaskHandle>& out) {
        out.clear();
        if (fast_successors_) {
            fast_take(&out);
            return;
        }
        lock();
        successors_closed_ = true;
        const std::size_t inline_count =
            (successor_count_ < kInlineSuccessors) ? successor_count_ : kInlineSuccessors;
        for (std::size_t i = 0; i < inline_count; ++i) {
            out.push_back(inline_successors_[i]);
        }
        for (TaskHandle handle : spilled_successors_) {
            out.push_back(handle);
        }
        spilled_successors_.clear();
        successor_count_ = 0;
        unlock();
    }

    // Returns the slot to a DORMANT state on release. The list stays CLOSED.
    //
    // This is the fix for a hang the simulation found. Reopening the list here
    // left a window in which the slot had a drained successor list, a still-old
    // generation, and closed_ == false - so a registration arriving in that window
    // was accepted by a task that had already finished and would never notify it.
    // The dependent then waited forever with pending_dependencies stuck at 1.
    //
    // A free slot has no successors and must accept none. It is reopened only by
    // open_successors(), when the pool hands it to a genuinely new task with a new
    // generation.
    void reset_successors() {
        if (fast_successors_) {
            // Already closed means a take (complete) closed AND drained it on this
            // same release path, so there is nothing to do. Otherwise this slot is
            // being released without completing (e.g. a failed submit): close it
            // and discard whatever registered.
            if ((succ_word_.load(std::memory_order_acquire) & kClosedBit) == 0) {
                fast_take(nullptr);
            }
            return;
        }
        lock();
        successors_closed_ = true;
        successor_count_   = 0;
        spilled_successors_.clear();
        unlock();
    }

    // Opens the list for a newly acquired task. Called by the pool on acquire,
    // after the generation has advanced, so no handle to the previous occupant
    // can pass the generation check in try_add_successor().
    void open_successors() {
        if (fast_successors_) {
            // The slot is exclusively ours and the list is empty: every path that
            // closes the word (complete's take, release's reset) drains the list
            // and waits out any registrant that had reserved a place. Publishing
            // the new epoch is therefore the whole of "open". A stale registrant
            // either still sees the old epoch / the closed bit, or sees the new
            // epoch and fails its generation compare.
            succ_word_.store(static_cast<std::uint64_t>(generation.load(std::memory_order_relaxed))
                                 << kEpochShift,
                             std::memory_order_release);
            return;
        }
        lock();
        successors_closed_ = false;
        successor_count_   = 0;
        spilled_successors_.clear();
        unlock();
    }

    [[nodiscard]] bool successors_spilled() const {
        return !spilled_successors_.empty() || successor_count_ > kInlineSuccessors;
    }

    // Selects the flag-word protocol (kOptSuccessorFastPath). Set once by the pool
    // before any worker exists; never changes afterwards.
    void set_fast_successors(bool on) {
        fast_successors_ = on;
        succ_word_.store(kClosedBit, std::memory_order_relaxed);
    }

private:
    // ---- fast successor protocol -------------------------------------------
    //
    // ONE atomic word is the single point of agreement between a registering
    // thread and the completing thread:
    //
    //     bits 63..32  epoch  = the generation of the task the list belongs to
    //     bit  31      closed
    //     bits 30..0   reserved = registrants that have committed a place
    //
    // Register: CAS (epoch==expected, !closed) -> reserved+1. Success is a
    //   commitment: the registrant WILL push, whatever happens next.
    // Complete/reset: exchange(closed) returns the last state any registrant
    //   could have committed into. reserved==0 means nobody ever can (later CASes
    //   see closed) - no lock, no list walk. reserved>0 means drain under the lock
    //   until that many entries have been collected; registrants that have
    //   reserved but not yet pushed are waited for (a bounded few instructions:
    //   they hold nothing and only need the spinlock).
    // A registrant that loses the CAS race to the exchange sees closed and reports
    // "already complete", exactly as before. There is no window in which a
    // registration is accepted but invisible to the drain.
    static constexpr std::uint64_t kClosedBit  = 1ull << 31;
    static constexpr std::uint64_t kCountMask  = kClosedBit - 1;
    static constexpr unsigned      kEpochShift = 32;

    bool fast_add(TaskHandle successor, std::uint32_t expected_generation) {
        std::uint64_t cur = succ_word_.load(std::memory_order_acquire);
        for (;;) {
            if ((cur >> kEpochShift) != expected_generation || (cur & kClosedBit) != 0) {
                return false;
            }
            if (succ_word_.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                break;
            }
        }
        lock();
        if (successor_count_ < kInlineSuccessors) {
            inline_successors_[successor_count_] = successor;
        } else {
            spilled_successors_.push_back(successor);
        }
        ++successor_count_;
        unlock();
        return true;
    }

    // out == nullptr discards (release path).
    void fast_take(std::vector<TaskHandle>* out) {
        const std::uint64_t prev = succ_word_.fetch_or(kClosedBit, std::memory_order_acq_rel);
        std::uint64_t remaining = prev & kCountMask;
        if ((prev & kClosedBit) != 0) {
            remaining = 0;  // already closed and drained by an earlier take
        }
        while (remaining != 0) {
            lock();
            const std::size_t inline_count =
                (successor_count_ < kInlineSuccessors) ? successor_count_ : kInlineSuccessors;
            const std::size_t total = successor_count_;
            if (out != nullptr) {
                for (std::size_t i = 0; i < inline_count; ++i) {
                    out->push_back(inline_successors_[i]);
                }
                for (TaskHandle handle : spilled_successors_) {
                    out->push_back(handle);
                }
            }
            spilled_successors_.clear();
            successor_count_ = 0;
            unlock();
            remaining -= (total < remaining) ? total : remaining;
        }
    }

    void lock() {
        while (successor_lock_.test_and_set(std::memory_order_acquire)) {
            // Held only across a push or a hand-off, so spinning is cheaper than
            // parking. Nothing blocking ever happens inside the critical section.
        }
    }
    void unlock() { successor_lock_.clear(std::memory_order_release); }

    std::atomic<std::uint64_t> succ_word_{kClosedBit};
    bool                       fast_successors_ = false;
    std::atomic_flag successor_lock_ = ATOMIC_FLAG_INIT;
    bool             successors_closed_ = false;
    std::size_t      successor_count_   = 0;
    TaskHandle       inline_successors_[kInlineSuccessors]{};
    std::vector<TaskHandle> spilled_successors_;
};
TB_END_CACHE_ALIGNED_TYPE

// Pinned so that growing a task is a deliberate act with a visible cost: the pool
// allocates capacity * sizeof(Task) up front, and task size is one of the
// variables the Phase E granularity experiment measures against.
static_assert(alignof(Task) == kCacheLineSize, "Task must not share a cache line");

} // namespace thunderbolt
