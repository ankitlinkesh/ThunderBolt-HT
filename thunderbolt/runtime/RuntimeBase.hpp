// Thunderbolt HT - machinery shared by every runtime implementation.
//
// WHY THIS EXISTS. The A/B comparison is only valid if StandardRuntime and
// ThunderboltRuntime differ in HOW work is distributed and in nothing else. Two
// separate implementations of "what does wait() mean", "when is a task complete",
// "how are dependencies resolved" and "what happens when the pool is exhausted"
// would be two chances to diverge - and any divergence shows up as a performance
// difference that gets misread as a scheduling result.
//
// So task lifetime, completion signalling, dependency resolution, waiter wakeup,
// help-on-wait policy and pool-exhaustion behaviour live here, once. A derived
// runtime supplies only dispatch: where a READY task goes, and how a worker finds
// the next one.
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
    // Deliberately final: identical semantics are the point, so a derived runtime
    // is not given the opportunity to redefine them.
    [[nodiscard]] TaskHandle submit(TaskDesc desc) final;
    [[nodiscard]] TaskHandle submit_after(TaskDesc desc, const TaskHandle* dependencies,
                                          std::size_t dependency_count) final;

    void               wait(TaskHandle handle) final;
    void               wait_all() final;
    [[nodiscard]] bool is_complete(TaskHandle handle) const final;

    using ITaskRuntime::submit;
    using ITaskRuntime::submit_after;

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

    // Dependency edges registered, and how many of those found their predecessor
    // already finished. A high already-satisfied ratio means the graph is being
    // built after the fact and is buying no parallelism.
    [[nodiscard]] std::uint64_t dependency_edge_count() const noexcept {
        return dependency_edges_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dependencies_already_satisfied() const noexcept {
        return dependencies_pre_satisfied_.load(std::memory_order_relaxed);
    }

protected:
    explicit RuntimeBase(RuntimeConfig config);
    ~RuntimeBase() override = default;

    // --- supplied by the derived runtime ---------------------------------

    // Makes a task whose dependencies are all satisfied available for execution.
    // This is the ONLY scheduling decision a derived runtime owns.
    virtual void enqueue_ready(TaskHandle handle, TaskPriority priority) = 0;

    // Executes one ready task if the derived runtime can find one. Returns false
    // when nothing is available right now. Used by help-on-wait, so it must not
    // block.
    virtual bool try_execute_one(TaskContext& ctx) = 0;

    // True when the calling thread is one of THIS runtime's workers.
    [[nodiscard]] virtual bool on_own_worker(std::uint32_t& out_index) const = 0;

    // --- provided to the derived runtime ---------------------------------

    // Runs a task to completion, releases dependents, and signals waiters.
    void execute(TaskHandle handle, TaskContext& ctx);

    [[nodiscard]] TaskPool&       pool() noexcept { return pool_; }
    [[nodiscard]] const TaskPool& pool() const noexcept { return pool_; }

    [[nodiscard]] const RuntimeConfig& config() const noexcept { return config_; }

    // Resolves the configured worker count, substituting the logical processor
    // count when 0 was requested. Shared so both runtimes interpret "auto"
    // identically - S58's sweep depends on the number meaning the same thing.
    [[nodiscard]] static std::uint32_t resolve_worker_count(const RuntimeConfig& config);

private:
    void complete(TaskHandle handle);

    // Decrements a dependent's counter and enqueues it if it just became ready.
    void release_dependent(TaskHandle dependent);

    void run_inline(TaskDesc&& desc);

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
    std::atomic<std::uint64_t> dependency_edges_{0};
    std::atomic<std::uint64_t> dependencies_pre_satisfied_{0};
};

} // namespace thunderbolt
