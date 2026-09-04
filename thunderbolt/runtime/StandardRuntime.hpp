// Thunderbolt HT - the baseline runtime.
//
// S11: this is a BASELINE, not a straw man. Beating a deliberately poor
// implementation would prove nothing, so this uses the design a competent
// engineer would reach for without a research agenda: long-lived worker threads,
// priority FIFO queues under a mutex, condition-variable parking, and no
// busy-waiting. Where a cheap improvement exists that any reasonable
// implementation would include, it is included.
//
// It shares observable semantics with ThunderboltRuntime exactly - including
// help-on-wait - because a semantic difference between the two would show up as
// a performance difference and be misread as a scheduling result.
#pragma once

#include <thunderbolt/api/ITaskRuntime.hpp>
#include <thunderbolt/api/RuntimeConfig.hpp>
#include <thunderbolt/core/task/TaskPool.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace thunderbolt {

class StandardRuntime final : public ITaskRuntime {
public:
    explicit StandardRuntime(RuntimeConfig config = {});
    ~StandardRuntime() override;

    // Bring the submit(callable) convenience overload back into scope; declaring
    // submit(TaskDesc) below would otherwise hide it.
    using ITaskRuntime::submit;

    [[nodiscard]] TaskHandle submit(TaskDesc desc) override;
    void                     wait(TaskHandle handle) override;
    void                     wait_all() override;
    [[nodiscard]] bool       is_complete(TaskHandle handle) const override;

    [[nodiscard]] std::uint32_t    worker_count() const noexcept override { return worker_count_; }
    [[nodiscard]] std::string_view name() const noexcept override { return "standard"; }

    // Tasks run inline because the pool was exhausted. Non-zero means the
    // configured task_capacity is too small for the workload; the run is still
    // correct, but its parallelism was reduced and results should say so.
    [[nodiscard]] std::uint64_t inline_execution_count() const noexcept {
        return inline_executions_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t outstanding_task_count() const noexcept {
        return outstanding_.load(std::memory_order_relaxed);
    }

private:
    void worker_loop(std::uint32_t worker_index);

    // Pops the highest-priority ready task, or an invalid handle if none.
    // Caller must hold queue_mutex_. Exists so the worker loop (which already
    // holds the lock for its condition-variable wait) and the help-on-wait path
    // share ONE scan rather than two copies that can drift apart - only one of
    // which any given test would exercise.
    [[nodiscard]] TaskHandle pop_locked();

    // Lock-taking wrapper around pop_locked(), for callers not already holding
    // queue_mutex_.
    [[nodiscard]] TaskHandle try_pop();

    // Runs one task if any is ready. Returns false when the queue was empty.
    bool try_execute_one(TaskContext& ctx);

    void execute(TaskHandle handle, TaskContext& ctx);
    void complete(TaskHandle handle);

    [[nodiscard]] bool is_complete_unlocked(TaskHandle handle) const;

    // True when the calling thread is one of THIS runtime's workers. Keyed on
    // the runtime instance, so nesting two runtimes does not confuse them.
    [[nodiscard]] bool on_own_worker(std::uint32_t& out_index) const;

    RuntimeConfig config_;
    std::uint32_t worker_count_ = 0;

    TaskPool pool_;

    // --- ready queues -----------------------------------------------------
    // One FIFO per priority (S13). Scanning five deques is cheaper than keeping
    // a heap ordered, and FIFO within a priority gives the aging behaviour that
    // keeps Background work from being starved outright.
    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<TaskHandle>  ready_[kPriorityCount];
    std::uint32_t           queued_count_ = 0;

    // --- completion -------------------------------------------------------
    // Separate from the queue lock: completion and dispatch are independent, and
    // sharing one lock would serialise them for no reason.
    mutable std::mutex      completion_mutex_;
    std::condition_variable completion_cv_;

    // Number of threads currently blocked in wait()/wait_all(). Lets the
    // completion path skip the mutex entirely when nobody is listening, which is
    // the common case and the difference between a competent baseline and a slow
    // one.
    std::atomic<std::uint32_t> waiters_{0};

    std::atomic<std::uint64_t> outstanding_{0};
    std::atomic<std::uint64_t> inline_executions_{0};

    std::atomic<bool>        running_{false};
    std::vector<std::thread> workers_;
};

} // namespace thunderbolt
