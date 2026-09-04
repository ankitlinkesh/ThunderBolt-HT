#include <thunderbolt/core/task/TaskPool.hpp>

#include <cassert>
#include <utility>

namespace thunderbolt {

TaskPool::TaskPool(std::uint32_t capacity)
    : capacity_(capacity), slots_(std::make_unique<Task[]>(capacity)) {
    free_list_.reserve(capacity);
    // Pushed in reverse so that the first acquisitions hand out slot 0, 1, 2...
    // Not required for correctness, but it makes pool behaviour readable in a
    // debugger and keeps early tasks contiguous in memory.
    for (std::uint32_t i = capacity; i > 0; --i) {
        free_list_.push_back(i - 1);
    }
}

TaskHandle TaskPool::acquire(TaskDesc&& desc) {
    std::uint32_t index = 0;
    {
        std::lock_guard lock(mutex_);
        if (free_list_.empty()) {
            return TaskHandle{};  // exhausted
        }
        index = free_list_.back();
        free_list_.pop_back();
    }

    Task& task = slots_[index];

    // The slot is exclusively ours here: it is off the free list and no handle
    // referring to its previous occupant can resolve, because release() already
    // advanced the generation.
    assert(task.state.load(std::memory_order_relaxed) == TaskState::Free);

    task.function       = std::move(desc.function);
    task.priority       = desc.priority;
    task.flags          = desc.flags;
    task.estimated_cost = desc.estimated_cost;

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

    // Advancing the generation is what invalidates every outstanding handle.
    // It must happen BEFORE the slot goes back on the free list: once it is on
    // the list another thread may acquire it, and a handle that still resolved
    // at that moment would resolve to someone else's task.
    task.generation.fetch_add(1, std::memory_order_relaxed);
    task.state.store(TaskState::Free, std::memory_order_release);

    std::lock_guard lock(mutex_);
    free_list_.push_back(handle.index);
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
    std::lock_guard lock(mutex_);
    return capacity_ - static_cast<std::uint32_t>(free_list_.size());
}

} // namespace thunderbolt
