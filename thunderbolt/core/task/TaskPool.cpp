#include <thunderbolt/core/task/TaskPool.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace thunderbolt {

// ---- per-thread slot cache (Phase I Stage 2) --------------------------------
//
// Each thread holds up to 2 * kMaxBatch free slot indices for ONE pool. acquire
// pops from it and release pushes to it with no lock and no atomics; the shard
// mutex is taken once per batch instead of once per task.
//
// Lifetime hazards, and how each is handled:
//  - A thread outliving its pool, or a new pool reusing the old one's address:
//    the cache is keyed by a never-reused id, and flushing goes through a
//    registry of LIVE pool ids, so a flush into a dead pool is dropped, not
//    performed. The registry mutex is held for the whole flush, and ~TaskPool
//    unregisters under the same mutex, so a pool cannot be destroyed mid-flush.
//  - One thread using two pools: the cache follows the pool used last, flushing
//    to the previous one when it is still alive.
//  - Slots hidden in a cache are invisible to other threads. That is bounded by
//    2 * batch per thread, and the batch is scaled down for small pools and off
//    entirely below kMinCapacityForCache.
//
// Memory ordering: no atomics are added or weakened. A slot only ever moves
// between threads through a shard mutex (spill -> refill), which is the same
// hand-off the uncached free list used.
namespace {

constexpr std::uint32_t kMaxBatch = 64;
constexpr std::uint32_t kMinCapacityForCache = 1024;

std::mutex& registry_mutex() {
    static std::mutex* m = new std::mutex;  // leaked: outlives every thread_local dtor
    return *m;
}
std::unordered_map<std::uint64_t, TaskPool*>& registry() {
    static auto* r = new std::unordered_map<std::uint64_t, TaskPool*>;
    return *r;
}
std::atomic<std::uint64_t> g_next_pool_id{1};

} // namespace

struct TaskPool::ThreadCache {
    std::uint64_t id    = 0;
    TaskPool*     pool  = nullptr;
    std::uint32_t count = 0;
    std::uint32_t slots[2 * kMaxBatch];

    ThreadCache() = default;
    ~ThreadCache() { TaskPool::drain_cache(*this); }
};

TaskPool::ThreadCache& TaskPool::thread_cache() noexcept {
    static thread_local ThreadCache cache;
    return cache;
}

void TaskPool::drain_cache(ThreadCache& cache) noexcept {
    if (cache.count != 0) {
        std::lock_guard registry_lock(registry_mutex());
        auto            it = registry().find(cache.id);
        if (it != registry().end()) {
            TaskPool&       pool  = *it->second;
            Shard&          shard = pool.shards_[preferred_shard()];
            std::lock_guard lock(shard.mutex);
            for (std::uint32_t i = 0; i < cache.count; ++i) {
                shard.free_list.push_back(cache.slots[i]);
            }
        }
    }
    cache.count = 0;
    cache.id    = 0;
    cache.pool  = nullptr;
}

void TaskPool::rebind_cache(ThreadCache& cache) noexcept {
    drain_cache(cache);
    cache.id   = id_;
    cache.pool = this;
}

void TaskPool::refill_cache(ThreadCache& cache) {
    const std::uint32_t home = preferred_shard();
    for (std::uint32_t offset = 0; offset < kShardCount; ++offset) {
        Shard&           shard = shards_[(home + offset) % kShardCount];
        std::unique_lock lock(shard.mutex, std::defer_lock);
        lock_counting(lock);
        if (shard.free_list.empty()) {
            if (offset == 0) {
                shard_misses_.increment();
            }
            continue;
        }
        while (cache.count < cache_batch_ && !shard.free_list.empty()) {
            cache.slots[cache.count++] = shard.free_list.back();
            shard.free_list.pop_back();
        }
        return;
    }
}

void TaskPool::spill_cache(ThreadCache& cache) noexcept {
    // Return the older half: the most recently released slots are the warmest and
    // stay for the next acquire.
    Shard&           shard = shards_[preferred_shard()];
    std::unique_lock lock(shard.mutex, std::defer_lock);
    lock_counting(lock);
    for (std::uint32_t i = 0; i < cache_batch_; ++i) {
        shard.free_list.push_back(cache.slots[i]);
    }
    for (std::uint32_t i = cache_batch_; i < cache.count; ++i) {
        cache.slots[i - cache_batch_] = cache.slots[i];
    }
    cache.count -= cache_batch_;
}

bool TaskPool::take_free_slot(std::uint32_t& index) {
    if (cache_batch_ != 0) {
        ThreadCache& cache = thread_cache();
        if (cache.id != id_) {
            rebind_cache(cache);
        }
        if (cache.count == 0) {
            refill_cache(cache);
        }
        if (cache.count == 0) {
            return false;
        }
        index = cache.slots[--cache.count];
        return true;
    }

    const std::uint32_t home = preferred_shard();

    // This thread's own shard first; the others only when it is empty. That
    // fallback is what keeps a fixed capacity usable when slots have pooled
    // unevenly - without it the pool would report exhaustion while thousands of
    // slots sat free in other shards.
    for (std::uint32_t offset = 0; offset < kShardCount; ++offset) {
        const std::uint32_t shard_index = (home + offset) % kShardCount;
        Shard&              shard       = shards_[shard_index];

        std::unique_lock lock(shard.mutex, std::defer_lock);
        lock_counting(lock);

        if (!shard.free_list.empty()) {
            index = shard.free_list.back();
            shard.free_list.pop_back();
            return true;
        } else if (offset == 0) {
            shard_misses_.increment();
        }
    }
    return false;
}

std::uint32_t TaskPool::preferred_shard() noexcept {
    // One assignment scheme for the whole runtime. This used to keep its own
    // thread-local counter, which put the same thread on unrelated slots in
    // unrelated structures for no benefit.
    static_assert(kShardCount == kCounterShards,
                  "free-list shards and counter shards share thread_slot(), so their counts "
                  "must agree or a thread would index out of range");
    return thread_slot();
}

TaskPool::TaskPool(std::uint32_t capacity, bool thread_cache, bool fast_successors)
    : capacity_(capacity),
      cache_batch_(thread_cache && capacity >= kMinCapacityForCache
                       ? std::min<std::uint32_t>(kMaxBatch, capacity / (kShardCount * 4))
                       : 0),
      id_(g_next_pool_id.fetch_add(1, std::memory_order_relaxed)),
      slots_(std::make_unique<Task[]>(capacity)),
      shards_(std::make_unique<Shard[]>(kShardCount)) {
    if (fast_successors) {
        for (std::uint32_t i = 0; i < capacity; ++i) {
            slots_[i].set_fast_successors(true);
        }
    }
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
    if (cache_batch_ != 0) {
        std::lock_guard lock(registry_mutex());
        registry()[id_] = this;
    }
}

TaskPool::~TaskPool() {
    if (cache_batch_ != 0) {
        std::lock_guard lock(registry_mutex());
        registry().erase(id_);
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

    std::uint32_t index = 0;
    if (!take_free_slot(index)) {
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

Task* TaskPool::reserve(TaskPriority priority, TaskHandle& out_handle) {
    acquire_count_.increment();

    std::uint32_t index = 0;
    if (!take_free_slot(index)) {
        out_handle = TaskHandle{};
        return nullptr;
    }

    Task& task = slots_[index];
    assert(task.state.load(std::memory_order_relaxed) == TaskState::Free);

    task.priority       = priority;
    task.flags          = TaskFlags::None;
    task.estimated_cost = 0;
    task.pending_dependencies.store(0, std::memory_order_relaxed);
    task.open_successors();

    out_handle = TaskHandle{index, task.generation.load(std::memory_order_relaxed)};
    return &task;
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
    if (cache_batch_ != 0) {
        ThreadCache& cache = thread_cache();
        if (cache.id != id_) {
            rebind_cache(cache);
        }
        if (cache.count == 2 * cache_batch_) {
            spill_cache(cache);
        }
        cache.slots[cache.count++] = handle.index;
        return;
    }

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
    if (cache_batch_ != 0) {
        // Slots parked in thread caches are free but on no shard list, so the
        // list sizes would over-report. State is exact for a quiescent pool.
        std::uint32_t live = 0;
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (slots_[i].state.load(std::memory_order_acquire) != TaskState::Free) {
                ++live;
            }
        }
        return live;
    }
    std::uint32_t free_total = 0;
    for (std::uint32_t shard = 0; shard < kShardCount; ++shard) {
        std::lock_guard lock(shards_[shard].mutex);
        free_total += static_cast<std::uint32_t>(shards_[shard].free_list.size());
    }
    return capacity_ - free_total;
}

} // namespace thunderbolt
