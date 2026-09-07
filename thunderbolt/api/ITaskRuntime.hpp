// Thunderbolt HT - the execution abstraction.
//
// This is the ONLY surface application code is permitted to depend on (S66).
// Every scheduling strategy hides behind it, which is what allows the identical
// workload to run under StandardRuntime and ThunderboltRuntime with no
// application change - the precondition for A/B benchmarking to mean anything
// at all (S57, S65, S94).
#pragma once

#include <thunderbolt/api/TaskDesc.hpp>
#include <thunderbolt/api/TaskHandle.hpp>
#include <thunderbolt/api/TaskTypes.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace thunderbolt {

class ITaskRuntime {
public:
    ITaskRuntime()                               = default;
    ITaskRuntime(const ITaskRuntime&)            = delete;
    ITaskRuntime& operator=(const ITaskRuntime&) = delete;
    virtual ~ITaskRuntime()                      = default;

    // ---------------------------------------------------------------------
    // Core interface. Implementations differ only in HOW work is distributed;
    // the observable semantics below are identical across runtimes by design,
    // because a semantic difference would invalidate every comparison made
    // between them.
    // ---------------------------------------------------------------------

    // Submits a task for execution. The returned handle may be waited on.
    // Thread-safe, and callable from inside a running task.
    [[nodiscard]] virtual TaskHandle submit(TaskDesc desc) = 0;

    // Submits a task that becomes runnable only once every handle in
    // `dependencies` has completed (S8).
    //
    // Dependencies are declared HERE, at submission, and can never be added
    // afterwards. That restriction is what makes a dependency cycle impossible to
    // express in the live runtime rather than merely detected in it: a task can
    // only depend on handles that already exist, and its own handle does not
    // exist until this call returns. S62 asks for cycle detection; being unable
    // to build one is the stronger property. (TaskGraph, which does allow edges
    // between existing nodes, checks for cycles explicitly - see taskgraph/.)
    //
    // Handles that have already completed - including stale ones - count as
    // satisfied, so passing them is harmless rather than a hang.
    [[nodiscard]] virtual TaskHandle submit_after(TaskDesc desc, const TaskHandle* dependencies,
                                                  std::size_t dependency_count) = 0;

    // Blocks until the referenced task has completed.
    //
    // Calling this from INSIDE a worker does not park the worker: the runtime
    // executes other ready tasks while waiting ("help-on-wait"). Blocking
    // instead would deadlock as soon as the pool is saturated with waiters,
    // and the alternative - a stackful fiber per task - is deferred. This is a
    // deliberate v1 decision, not an implementation accident.
    //
    // A stale handle (its task completed and its pool slot was recycled) is
    // treated as complete, which is the only answer that is both safe and true.
    virtual void wait(TaskHandle handle) = 0;

    // Blocks until no submitted task remains outstanding.
    virtual void wait_all() = 0;

    // Non-blocking completion query. Same stale-handle rule as wait().
    [[nodiscard]] virtual bool is_complete(TaskHandle handle) const = 0;

    // Number of worker threads. Does not count the calling thread.
    [[nodiscard]] virtual std::uint32_t worker_count() const noexcept = 0;

    // Stable identifier recorded in benchmark output (S87).
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // ---------------------------------------------------------------------
    // Convenience layer. Non-virtual on purpose: these are expressed purely in
    // terms of the interface above, so every runtime gets identical behaviour
    // here for free and cannot accidentally diverge in a way that would skew a
    // comparison.
    // ---------------------------------------------------------------------

    // Submits a callable, which may take TaskContext& or nothing.
    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, TaskDesc>)
    [[nodiscard]] TaskHandle submit(F&& fn, TaskPriority priority = TaskPriority::Normal) {
        return submit(TaskDesc{TaskFunction{std::forward<F>(fn)}, priority});
    }

    // Convenience: submit a callable that runs after the listed dependencies.
    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, TaskDesc>)
    [[nodiscard]] TaskHandle submit_after(std::initializer_list<TaskHandle> dependencies, F&& fn,
                                          TaskPriority priority = TaskPriority::Normal) {
        return submit_after(TaskDesc{TaskFunction{std::forward<F>(fn)}, priority},
                            dependencies.begin(), dependencies.size());
    }

    [[nodiscard]] TaskHandle submit_after(const std::vector<TaskHandle>& dependencies,
                                          TaskDesc                       desc) {
        return submit_after(std::move(desc), dependencies.data(), dependencies.size());
    }

    // Executes body(begin_i, end_i) over [begin, end) in chunks of at most
    // `grain` items, and blocks until every chunk completes.
    //
    // DYNAMIC PARTITIONING (Phase I Stage 6). This does NOT submit one task per
    // chunk. It spawns at most min(batch_count, worker_count()) tasks, and each
    // one repeatedly claims the next `grain`-sized chunk from a shared atomic
    // cursor until the range is exhausted. That turns an O(items/grain) task
    // count into O(workers): at grain=1 over a million items this submits a
    // handful of tasks, not a million, which is what lets it compete with a
    // partitioner (e.g. Taskflow's for_each) instead of only with hand-written
    // explicit-task loops. Load balances by claiming rather than by work-stealing
    // - no deque, no victim selection, just one contended fetch_add.
    //
    // SAFE UNDER THE DETERMINISM RULE ONLY FOR ELEMENT-WISE WORK. Every call this
    // project makes writes out[i] as a function of index i alone, so which task
    // claims which chunk can never change the result. A REDUCTION over
    // dynamically-claimed chunks would NOT be safe - the combine order would
    // depend on scheduling timing - and must not be expressed through this
    // function; that is a caller obligation this function cannot check.
    //
    // `grain` is still an explicit caller decision (S15/S85): it sets the claim
    // size, trading cursor-contention (small grain) against load imbalance in the
    // tail (large grain). Automatic grain selection remains a Phase G experiment.
    template <typename F>
    void parallel_for(std::size_t begin, std::size_t end, std::size_t grain, F&& body,
                      TaskPriority priority = TaskPriority::Normal) {
        if (begin >= end) {
            return;
        }
        if (grain == 0) {
            grain = 1;
        }

        // `body` outlives the tasks because this function waits before returning,
        // so capturing a pointer to it (and to `cursor` below) is safe.
        auto* body_ptr = &body;

        const std::size_t count       = end - begin;
        const std::size_t batch_count = (count + grain - 1) / grain;

        const std::uint32_t configured_workers = worker_count();
        const std::size_t   worker_cap = (configured_workers == 0) ? 1 : configured_workers;
        const std::size_t   task_count = (batch_count < worker_cap) ? batch_count : worker_cap;

        std::atomic<std::size_t> cursor{begin};

        // Rarely exercised now that task_count is bounded by worker_count() rather
        // than by batch_count, but kept: an exotic configuration with more workers
        // than kInlineBatchHandles must still be correct, not merely fast.
        TaskHandle inline_handles[kInlineBatchHandles];
        std::vector<TaskHandle> spilled;
        if (task_count > kInlineBatchHandles) {
            spilled.resize(task_count);
        }
        TaskHandle* handles = (task_count > kInlineBatchHandles) ? spilled.data() : inline_handles;

        for (std::size_t t = 0; t < task_count; ++t) {
            handles[t] = submit([body_ptr, &cursor, end, grain](TaskContext& ctx) {
                for (;;) {
                    const std::size_t lo = cursor.fetch_add(grain, std::memory_order_relaxed);
                    if (lo >= end) {
                        break;
                    }
                    const std::size_t hi = (lo + grain < end) ? (lo + grain) : end;
                    if constexpr (std::is_invocable_v<F&, std::size_t, std::size_t, TaskContext&>) {
                        (*body_ptr)(lo, hi, ctx);
                    } else {
                        (void)ctx;  // body did not ask for a context
                        (*body_ptr)(lo, hi);
                    }
                }
            }, priority);
        }

        for (std::size_t t = 0; t < task_count; ++t) {
            wait(handles[t]);
        }
    }

private:
    // Covers the batch counts a frame graph actually produces without touching
    // the allocator on the submission path.
    static constexpr std::size_t kInlineBatchHandles = 64;
};

} // namespace thunderbolt
