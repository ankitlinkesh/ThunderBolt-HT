// Thunderbolt HT - pooled task storage.
#pragma once

#include <thunderbolt/api/TaskDesc.hpp>
#include <thunderbolt/api/TaskHandle.hpp>
#include <thunderbolt/core/task/Task.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace thunderbolt {

// A fixed-capacity slab of task slots with a free list.
//
// Fixed capacity is a deliberate constraint, not a limitation left unfixed:
// growing the slab would invalidate the pointer arithmetic that makes slot
// lookup a single indexed load, and a runtime that silently allocates under
// load is a runtime whose benchmark results include the allocator.
//
// The free list is guarded by a mutex. That is very likely to become a
// contention point once Phase C has eight workers submitting concurrently - and
// it is left as a mutex anyway, because S60 puts correctness first and S95.10
// says prefer the simple mechanism until a measurement justifies complexity.
// Phase E's profiler is what will decide whether this needs to be lock-free.
class TaskPool {
public:
    explicit TaskPool(std::uint32_t capacity);

    TaskPool(const TaskPool&)            = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    // Takes a free slot and moves `desc` into it. Returns an invalid handle if
    // the pool is exhausted; callers must handle that rather than assume it
    // cannot happen.
    [[nodiscard]] TaskHandle acquire(TaskDesc&& desc);

    // Returns the slot to the free list and bumps its generation, invalidating
    // every outstanding handle to it. Must be called exactly once per acquire.
    void release(TaskHandle handle);

    // Returns the task for `handle`, or nullptr when the handle is stale - that
    // is, when the slot has since been recycled. A stale handle is not an error:
    // it is how "this task finished a while ago" is represented.
    [[nodiscard]] Task*       get(TaskHandle handle) noexcept;
    [[nodiscard]] const Task* get(TaskHandle handle) const noexcept;

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

    // Slots currently acquired. Intended for tests and for the Phase E profiler,
    // not for scheduling decisions.
    [[nodiscard]] std::uint32_t live_count() const;

private:
    std::uint32_t capacity_;

    // unique_ptr<Task[]> rather than vector<Task>: Task holds atomics and is
    // therefore neither copyable nor movable, which vector's growth path needs.
    std::unique_ptr<Task[]> slots_;

    mutable std::mutex         mutex_;
    std::vector<std::uint32_t> free_list_;
};

} // namespace thunderbolt
