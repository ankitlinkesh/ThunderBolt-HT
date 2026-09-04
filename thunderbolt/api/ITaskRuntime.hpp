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
