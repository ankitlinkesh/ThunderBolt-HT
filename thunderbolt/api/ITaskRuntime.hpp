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

    // Executes body(begin_i, end_i) over [begin, end) in batches of at most
    // `grain` items, and blocks until all batches complete.
    //
    // Batching rather than one task per item is the whole point (S15): a task
    // per item makes scheduler overhead dominate, which is the effect Phase E's
    // granularity experiment exists to measure rather than assume.
    //
    // `grain` is an explicit caller decision here. Automatic grain selection is
    // a Phase G experiment and must be justified by measurement before it hides
    // this choice.
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
        // so capturing a pointer to it is safe and keeps the task body inline.
        auto* body_ptr = &body;

        const std::size_t count       = end - begin;
        const std::size_t batch_count = (count + grain - 1) / grain;

        TaskHandle inline_handles[kInlineBatchHandles];
        std::vector<TaskHandle> spilled;
        if (batch_count > kInlineBatchHandles) {
            spilled.resize(batch_count);
        }
        TaskHandle* handles = (batch_count > kInlineBatchHandles) ? spilled.data() : inline_handles;

        for (std::size_t b = 0; b < batch_count; ++b) {
            const std::size_t lo = begin + b * grain;
            const std::size_t hi = (lo + grain < end) ? (lo + grain) : end;
            handles[b] = submit([body_ptr, lo, hi](TaskContext& ctx) {
                if constexpr (std::is_invocable_v<F&, std::size_t, std::size_t, TaskContext&>) {
                    (*body_ptr)(lo, hi, ctx);
                } else {
                    (void)ctx;  // body did not ask for a context
                    (*body_ptr)(lo, hi);
                }
            }, priority);
        }

        for (std::size_t b = 0; b < batch_count; ++b) {
            wait(handles[b]);
        }
    }

private:
    // Covers the batch counts a frame graph actually produces without touching
    // the allocator on the submission path.
    static constexpr std::size_t kInlineBatchHandles = 64;
};

} // namespace thunderbolt
