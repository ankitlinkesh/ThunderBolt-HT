#include <thunderbolt/core/task/TaskPool.hpp>

#include <atomic>
#include <cassert>
#include <utility>

namespace thunderbolt {

std::uint32_t TaskPool::preferred_shard() noexcept {
    // One assignment scheme for the whole runtime. This used to keep its own
    // thread-local counter, which put the same thread on unrelated slots in
    // unrelated structures for no benefit.
    static_assert(kShardCount == kCounterShards,
                  "free-list shards and counter shards share thread_slot(), so their counts "
                  "must agree or a thread would index out of range");
    return thread_slot();
}

TaskPool::TaskPool(std::uint32_t capacity)
    : capacity_(capacity),
      slots_(std::make_unique<Task[]>(capacity)),
      shards_(std::make_unique<Shard[]>(kShardCount)) {
    for (std::uint32_t shard = 0; shard < kShardCount; ++shard) {
        shards_[shard].free_list.reserve(capacity / kShardCount + 1);
    }
    // Dealt round-robin so every shard starts with roughly capacity/kShardCount
    // entries and no thread begins by missing. Descending, so the first slots
    // handed out are low-numbered and early tasks stay contiguous in memory.
    for (std::uint32_t i = capacity; i > 0; --i) {
        const std::uint32_t index = i - 1;
        shards_[index % kShardCount].free_list.push_back(index);
    }
}

void TaskPool::lock_counting(std::unique_lock<std::mutex>& lock) const {
    // try_lock first: an uncontended acquisition costs the same as a plain lock,
    // and a failed attempt is exactly the definition of contention.
    if (!lock.try_lock()) {
        contended_locks_.increment();
        lock.lock();
    }
}

TaskHandle TaskPool::acquire(TaskDesc&& desc) {
    acquire_count_.increment();

    const std::uint32_t home = preferred_shard();

    std::uint32_t index = 0;
    bool          found = false;

    // This thread's own shard first; the others only when it is empty. That
    // fallback is what keeps a fixed capacity usable when slots have pooled
    // unevenly - without it the pool would report exhaustion while thousands of
    // slots sat free in other shards.
    for (std::uint32_t offset = 0; offset < kShardCount && !found; ++offset) {
        const std::uint32_t shard_index = (home + offset) % kShardCount;
        Shard&              shard       = shards_[shard_index];

        std::unique_lock lock(shard.mutex, std::defer_lock);
        lock_counting(lock);

        if (!shard.free_list.empty()) {
            index = shard.free_list.back();
            shard.free_list.pop_back();
            found = true;
        } else if (offset == 0) {
            shard_misses_.increment();
        }
    }

    if (!found) {
        return TaskHandle{};  // every shard empty: genuinely exhausted
    }

    Task& task = slots_[index];

    // The slot is exclusively ours here: it is off the free list, and no handle
    // referring to its previous occupant can resolve, because release() already
    // advanced the generation.
    assert(task.state.load(std::memory_order_relaxed) == TaskState::Free);

    task.function       = std::move(desc.function);
    task.priority       = desc.priority;
    task.flags          = desc.flags;
    task.estimated_cost = desc.estimated_cost;
    task.pending_dependencies.store(0, std::memory_order_relaxed);

    // Opened only now, for a task that already has its new generation. A slot on
    // the free list must refuse successor registrations; see reset_successors().
    task.open_successors();

    const std::uint32_t generation = task.generation.load(std::memory_order_relaxed);

    // Release: everything written above must be visible to any thread that
    // subsequently observes this state.
    task.state.store(TaskState::Created, std::memory_order_release);

    return TaskHandle{index, generation};
}

void TaskPool::release(TaskHandle handle) {
    assert(handle.valid());
    assert(handle.index < capacity_);

    Task& task = slots_[handle.index];
    assert(task.generation.load(std::memory_order_relaxed) == handle.generation &&
           "double release, or release of a stale handle");

    // Destroy the body before the slot becomes reusable, so any state it captured
    // is released at a predictable point rather than whenever the slot happens to
    // be handed out again.
    task.function.reset();

    // Advance the generation BEFORE touching the successor list. Both orderings
    // matter and for different reasons:
    //
    //  - Before the free list, because once listed another thread may acquire the
    //    slot, and a handle that still resolved then would resolve to someone
    //    else's task.
    //  - Before reset_successors(), because the reset used to reopen the list
    //    while the generation was still old, letting a registration attach to a
    //    task that had already drained its successors and would never notify it.
    task.generation.fetch_add(1, std::memory_order_release);

    // Returns the slot to a dormant, CLOSED state. It is reopened at acquire.
    task.reset_successors();
    task.state.store(TaskState::Free, std::memory_order_release);

    release_count_.increment();

    // Returned to the RELEASING thread's shard, not the slot's original one. A
    // worker that consumes and completes tasks therefore recycles through its own
    // sublist and rarely touches another shard's lock.
    Shard&           shard = shards_[preferred_shard()];
    std::unique_lock lock(shard.mutex, std::defer_lock);
    lock_counting(lock);
    shard.free_list.push_back(handle.index);
}

Task* TaskPool::get(TaskHandle handle) noexcept {
    if (!handle.valid() || handle.index >= capacity_) {
        return nullptr;
    }
    Task& task = slots_[handle.index];
    if (task.generation.load(std::memory_order_acquire) != handle.generation) {
        return nullptr;  // stale: slot recycled since the handle was issued
    }
    return &task;
}

const Task* TaskPool::get(TaskHandle handle) const noexcept {
    return const_cast<TaskPool*>(this)->get(handle);
}

std::uint32_t TaskPool::live_count() const {
    std::uint32_t free_total = 0;
    for (std::uint32_t shard = 0; shard < kShardCount; ++shard) {
        std::lock_guard lock(shards_[shard].mutex);
        free_total += static_cast<std::uint32_t>(shards_[shard].free_list.size());
    }
    return capacity_ - free_total;
}

} // namespace thunderbolt
