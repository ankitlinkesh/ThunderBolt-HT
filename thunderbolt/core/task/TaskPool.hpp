// Thunderbolt HT - pooled task storage.
#pragma once

#include <thunderbolt/api/TaskDesc.hpp>
#include <thunderbolt/api/TaskHandle.hpp>
#include <thunderbolt/core/task/Task.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace thunderbolt {

// A fixed-capacity slab of task slots with a SHARDED free list.
//
// Fixed capacity is deliberate: growing the slab would invalidate the pointer
// arithmetic that makes slot lookup a single indexed load, and a runtime that
// silently allocates under load is a runtime whose benchmark results include the
// allocator.
//
// WHY SHARDED. The free list began as one mutex, on the reasoning that
// correctness comes first and simple mechanisms should stand until a measurement
// says otherwise (S60, S95.10). The measurement arrived: at 65536 tasks the
// granularity benchmark showed 52.8% of acquisitions CONTENDED, and per-task cost
// roughly 4x an external reference scheduler. A contended mutex costs microseconds
// because it parks the thread, and the pool is touched twice per task.
//
// Sharding splits the free list into independent sublists, so workers usually
// touch different locks. It is a smaller change than a lock-free stack, and
// without ThreadSanitizer available that risk asymmetry matters more than the
// last few percent.
class TaskPool {
public:
    // Independent free-list shards. Above the logical processor count of the
    // reference machine, so distinct workers rarely collide, and a power of two
    // so shard selection is a mask.
    static constexpr std::uint32_t kShardCount = 16;

    explicit TaskPool(std::uint32_t capacity);

    TaskPool(const TaskPool&)            = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    // Takes a free slot and moves `desc` into it. Returns an invalid handle when
    // every shard is empty; callers must handle that rather than assume it cannot
    // happen.
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

    // Raw slot access for diagnostics only. Bypasses the generation check on
    // purpose: the point is to inspect slots whose handles no longer resolve.
    [[nodiscard]] const Task* slot_for_diagnostics(std::uint32_t index) const noexcept {
        return (index < capacity_) ? &slots_[index] : nullptr;
    }

    // Slots currently acquired. For tests and profiling, not for scheduling.
    [[nodiscard]] std::uint32_t live_count() const;

    // --- contention instrumentation ---------------------------------------
    // These are what turned "the pool mutex is probably the bottleneck" from a
    // guess into a measurement, and they stay so the next claim about it can be
    // checked the same way.
    [[nodiscard]] std::uint64_t acquire_count() const noexcept {
        return acquire_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t release_count() const noexcept {
        return release_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t contended_lock_count() const noexcept {
        return contended_locks_.load(std::memory_order_relaxed);
    }
    // Times a shard was empty and another had to be searched. Persistent
    // non-zero values mean slots are pooling unevenly and the shard count or
    // assignment needs revisiting.
    [[nodiscard]] std::uint64_t shard_miss_count() const noexcept {
        return shard_misses_.load(std::memory_order_relaxed);
    }

private:
    // One shard per cache line: two shards sharing a line would reintroduce, as
    // false sharing, exactly the contention the sharding removes.
    TB_BEGIN_CACHE_ALIGNED_TYPE
    struct alignas(kCacheLineSize) Shard {
        std::mutex                 mutex;
        std::vector<std::uint32_t> free_list;
    };
    TB_END_CACHE_ALIGNED_TYPE

    // Shard this thread should prefer. Stable per thread, so a worker keeps
    // returning to the same sublist and its slots stay warm in that core's cache.
    [[nodiscard]] static std::uint32_t preferred_shard() noexcept;

    // Locks `shard`, counting the acquisition as contended when it could not be
    // taken immediately.
    void lock_counting(std::unique_lock<std::mutex>& lock) const;

    std::uint32_t capacity_;

    // unique_ptr<Task[]> rather than vector<Task>: Task holds atomics and is
    // therefore neither copyable nor movable, which vector's growth path needs.
    std::unique_ptr<Task[]> slots_;

    std::unique_ptr<Shard[]> shards_;

    mutable std::atomic<std::uint64_t> acquire_count_{0};
    mutable std::atomic<std::uint64_t> release_count_{0};
    mutable std::atomic<std::uint64_t> contended_locks_{0};
    mutable std::atomic<std::uint64_t> shard_misses_{0};
};

} // namespace thunderbolt
