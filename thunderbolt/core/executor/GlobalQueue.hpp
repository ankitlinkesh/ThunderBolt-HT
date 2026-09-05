// Thunderbolt HT - global injection queue.
//
// S9 is explicit that a globally contended queue must not be the primary
// execution mechanism; per-worker deques are. This queue exists for the two
// cases those deques cannot serve:
//
//   1. Work submitted from OUTSIDE the runtime, which has no local deque.
//   2. Overflow when a worker's bounded deque is full.
//
// Both are off the hot path by construction, so a mutex is the right tool. Making
// this lock-free would add risk to a structure that, if it ever shows up in a
// profile, is telling us the local deques are too small rather than that the
// queue is too slow.
#pragma once

#include <thunderbolt/api/TaskHandle.hpp>
#include <thunderbolt/api/TaskTypes.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace thunderbolt {

class GlobalQueue {
public:
    void push(TaskHandle handle, TaskPriority priority) {
        std::lock_guard lock(mutex_);
        queues_[static_cast<std::size_t>(priority)].push_back(handle);
        count_.fetch_add(1, std::memory_order_release);
    }

    // Returns an invalid handle when empty. Scans highest priority first by
    // default, so ordering matches StandardRuntime's - a divergence here would
    // show up as a scheduling result rather than the semantic difference it is.
    //
    // `lowest_first` inverts the scan for aging. It has to exist: externally
    // submitted work lands HERE, not on a worker deque, so an aging rotation
    // applied only to the deques would leave exactly the tasks a user submitted
    // subject to strict priority - which is the starvation the mode exists to
    // prevent. Found by the starvation test failing.
    [[nodiscard]] TaskHandle pop(bool lowest_first = false) {
        // Unsynchronised early-out. A false negative is harmless: the caller is
        // about to try stealing anyway, and a task left here will be found on the
        // next pass or by another worker.
        if (count_.load(std::memory_order_acquire) == 0) {
            return TaskHandle{};
        }

        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < kPriorityCount; ++i) {
            const std::size_t p = lowest_first ? (kPriorityCount - 1 - i) : i;
            if (!queues_[p].empty()) {
                TaskHandle handle = queues_[p].front();
                queues_[p].pop_front();
                count_.fetch_sub(1, std::memory_order_release);
                return handle;
            }
        }
        return TaskHandle{};
    }

    // A hint. Used to decide whether a worker may park, never for correctness.
    [[nodiscard]] std::uint64_t size_hint() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex     mutex_;
    std::deque<TaskHandle> queues_[kPriorityCount];

    // Kept outside the lock so the common "is there anything here?" check made by
    // every idle worker does not serialise on the mutex.
    std::atomic<std::uint64_t> count_{0};
};

} // namespace thunderbolt
