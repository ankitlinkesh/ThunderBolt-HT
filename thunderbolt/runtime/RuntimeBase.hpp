// Thunderbolt HT - machinery shared by every runtime implementation.
//
// WHY THIS EXISTS. The A/B comparison is only valid if StandardRuntime and
// ThunderboltRuntime differ in HOW work is distributed and in nothing else. Two
// separate implementations of "what does wait() mean", "when is a task complete",
// and "what happens when the pool is exhausted" would be two chances to diverge -
// and any divergence shows up as a performance difference that gets misread as a
// scheduling result.
//
// So task lifetime, completion signalling, waiter wakeup, help-on-wait policy and
// pool-exhaustion behaviour live here, once. A derived runtime supplies only
// dispatch: where a submitted task goes, and how a worker finds the next one.
#pragma once

#include <thunderbolt/api/ITaskRuntime.hpp>
#include <thunderbolt/api/RuntimeConfig.hpp>
#include <thunderbolt/core/task/TaskPool.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace thunderbolt {

class RuntimeBase : public ITaskRuntime {
public:
    // These are deliberately NOT virtual beyond this class: identical semantics
    // are the point, so a derived runtime is not given the opportunity to
    // redefine them.
    void               wait(TaskHandle handle) final;
    void               wait_all() final;
    [[nodiscard]] bool is_complete(TaskHandle handle) const final;

    // Tasks that ran inline because the pool was exhausted. Non-zero means the
    // configured task_capacity was too small: the run stayed correct, but its
    // parallelism was reduced and any result must say so.
    [[nodiscard]] std::uint64_t inline_execution_count() const noexcept {
        return inline_executions_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t outstanding_task_count() const noexcept {
        return outstanding_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t completed_task_count() const noexcept {
        return completed_.load(std::memory_order_relaxed);
    }

protected:
    explicit RuntimeBase(RuntimeConfig config);
    ~RuntimeBase() override = default;

    // --- supplied by the derived runtime ---------------------------------

    // Executes one ready task if the derived runtime can find one. Returns false
    // when there is nothing available right now. Used by help-on-wait, so it must
    // not block.
    virtual bool try_execute_one(TaskContext& ctx) = 0;

    // True when the calling thread is one of THIS runtime's workers.
    [[nodiscard]] virtual bool on_own_worker(std::uint32_t& out_index) const = 0;

    // --- provided to the derived runtime ---------------------------------

    // Reserves a pool slot and counts the task as outstanding. Returns an invalid
    // handle when the pool is exhausted, in which case the caller should hand the
    // description to run_inline().
    [[nodiscard]] TaskHandle acquire_task(TaskDesc&& desc);

    // Pool-exhaustion fallback: runs the task on the calling thread and clears the
    // outstanding count that acquire_task() added.
    void run_inline(TaskDesc&& desc);

    // Runs a task to completion and signals waiters.
    void execute(TaskHandle handle, TaskContext& ctx);

    [[nodiscard]] TaskPool&       pool() noexcept { return pool_; }
    [[nodiscard]] const TaskPool& pool() const noexcept { return pool_; }

    [[nodiscard]] const RuntimeConfig& config() const noexcept { return config_; }

    // Resolves the configured worker count, substituting the logical processor
    // count when 0 was requested. Shared so both runtimes interpret "auto"
    // identically - S58's sweep depends on the number meaning the same thing.
    [[nodiscard]] static std::uint32_t resolve_worker_count(const RuntimeConfig& config);

private:
    void               complete(TaskHandle handle);
    [[nodiscard]] bool is_complete_internal(TaskHandle handle) const;

    RuntimeConfig config_;
    TaskPool      pool_;

    mutable std::mutex      completion_mutex_;
    std::condition_variable completion_cv_;

    // Threads currently blocked in wait()/wait_all(). Lets the completion path
    // skip the mutex entirely when nobody is listening, which is almost every
    // completion under load.
    std::atomic<std::uint32_t> waiters_{0};

    std::atomic<std::uint64_t> outstanding_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> inline_executions_{0};
};

} // namespace thunderbolt
