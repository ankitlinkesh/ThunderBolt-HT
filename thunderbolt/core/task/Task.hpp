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

    // Returns the slot to a reusable state. Called by the pool on release.
    void reset_successors() {
        lock();
        successors_closed_ = false;
        successor_count_   = 0;
        spilled_successors_.clear();
        unlock();
    }

    [[nodiscard]] bool successors_spilled() const {
        return !spilled_successors_.empty() || successor_count_ > kInlineSuccessors;
    }

private:
    void lock() {
        while (successor_lock_.test_and_set(std::memory_order_acquire)) {
            // Held only across a push or a hand-off, so spinning is cheaper than
            // parking. Nothing blocking ever happens inside the critical section.
        }
    }
    void unlock() { successor_lock_.clear(std::memory_order_release); }

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
