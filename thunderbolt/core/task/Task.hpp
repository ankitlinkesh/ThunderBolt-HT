// Thunderbolt HT - the internal task record.
//
// Lives under core/, not api/: application code names TaskHandle and TaskDesc,
// never this (S66).
#pragma once

#include <thunderbolt/api/TaskFunction.hpp>
#include <thunderbolt/api/TaskTypes.hpp>

#include <atomic>
#include <cstdint>

namespace thunderbolt {

// One pooled task slot.
//
// Cache-line aligned because `state` is written by whichever worker executes the
// task while neighbouring slots are being written by other workers. Without the
// alignment, tasks that share a line would ping-pong that line between cores on
// every state transition - a slowdown that looks exactly like poor scheduling
// while actually being layout.
//
// MSVC warns (C4324) that the alignment introduces padding. That padding IS the
// feature: it is what keeps two tasks off one line. Suppressed here rather than
// project-wide, so the warning still fires anywhere it would be a surprise.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4324)
#endif

struct alignas(kCacheLineSize) Task {
    TaskFunction function;

    // Generation of the handle currently occupying this slot. Incremented on
    // release, which is what makes a handle to a recycled slot detectably stale
    // rather than silently pointing at someone else's task (S62).
    std::atomic<std::uint32_t> generation{0};

    // S7 lifecycle position.
    std::atomic<TaskState> state{TaskState::Free};

    TaskPriority  priority       = TaskPriority::Normal;
    TaskFlags     flags          = TaskFlags::None;
    std::uint32_t estimated_cost = 0;

    Task()                       = default;
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;
};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

// Pinned so that growing a task is a deliberate act with a visible cost: the
// pool allocates capacity * sizeof(Task) up front, and task size is one of the
// variables the Phase E granularity experiment is measuring against.
static_assert(sizeof(Task) == 2 * kCacheLineSize,
              "Task size changed. Re-measure before accepting it.");
static_assert(alignof(Task) == kCacheLineSize, "Task must not share a cache line");

} // namespace thunderbolt
