#include <thunderbolt/core/task/TaskPool.hpp>

#include <atomic>
#include <cassert>
#include <utility>

namespace thunderbolt {
namespace {

// Stable per-thread shard assignment, handed out round-robin on first use. A hash
// of the thread id would also work but distributes unpredictably; a counter keeps
// distinct threads on distinct shards for as many threads as there are shards.
std::atomic<std::uint32_t> g_next_shard{0};

} // namespace

std::uint32_t TaskPool::preferred_shard() noexcept {
    static thread_local const std::uint32_t shard =
        g_next_shard.fetch_add(1, std::memory_order_relaxed) % kShardCount;
    return shard;
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
        contended_locks_.fetch_add(1, std::memory_order_relaxed);
        lock.lock();
    }
}

TaskHandle TaskPool::acquire(TaskDesc&& desc) {
    acquire_count_.fetch_add(1, std::memory_order_relaxed);

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
            shard_misses_.fetch_add(1, std::memory_order_relaxed);
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

    // The successor list must already have been closed and drained by the
    // completing thread; this returns it to a reusable state.
    task.reset_successors();

    // Advancing the generation is what invalidates every outstanding handle. It
    // must happen BEFORE the slot goes back on a free list: once it is listed,
    // another thread may acquire it, and a handle that still resolved at that
    // moment would resolve to someone else's task.
    task.generation.fetch_add(1, std::memory_order_relaxed);
    task.state.store(TaskState::Free, std::memory_order_release);

    release_count_.fetch_add(1, std::memory_order_relaxed);

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
