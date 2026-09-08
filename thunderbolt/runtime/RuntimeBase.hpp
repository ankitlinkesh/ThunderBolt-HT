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
#include <thunderbolt/core/ShardedCounter.hpp>
#include <thunderbolt/core/task/TaskPool.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace thunderbolt {

// Cache-line-aligned members below deliberately pad this class; that padding is
// the point, so C4324 is suppressed here rather than project-wide.
TB_BEGIN_CACHE_ALIGNED_TYPE

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
        return inline_executions_.unsigned_sum();
    }

    [[nodiscard]] std::uint64_t outstanding_task_count() const noexcept {
        return outstanding_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t completed_task_count() const noexcept {
        return completed_.unsigned_sum();
    }

    // Tasks whose callable threw. The runtime catches at the worker boundary
    // and keeps running - see execute()'s comment - so this is the caller's way
    // to learn a task silently didn't finish its work, instead of grepping
    // stderr for it after the fact.
    [[nodiscard]] std::uint64_t uncaught_exception_count() const noexcept {
        return uncaught_exceptions_.unsigned_sum();
    }

    // Dependency edges registered, and how many of those found their predecessor
    // already finished. A high already-satisfied ratio means the graph is being
    // built after the fact and is buying no parallelism.
    [[nodiscard]] std::uint64_t dependency_edge_count() const noexcept {
        return dependency_edges_.unsigned_sum();
    }
    [[nodiscard]] std::uint64_t dependencies_already_satisfied() const noexcept {
        return dependencies_pre_satisfied_.unsigned_sum();
    }

    // Task-pool free-list statistics. Exposed because the pool mutex is taken
    // twice per task and is a prime suspect whenever per-task cost looks too
    // high - and a suspect should be checked against a counter, not reasoned
    // about.
    [[nodiscard]] std::uint64_t pool_acquire_count() const noexcept {
        return pool_.acquire_count();
    }
    [[nodiscard]] std::uint64_t pool_contended_lock_count() const noexcept {
        return pool_.contended_lock_count();
    }

    // Diagnostic: writes every task slot that is not free, with its lifecycle
    // state and outstanding dependency count, to stderr. Distinguishes a task
    // that never became runnable (still Waiting - a lost dependency decrement)
    // from one that was made runnable but never picked up (Queued - a lost
    // wakeup). Those need opposite fixes, so guessing between them is expensive.
    void dump_outstanding() const;

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

    // Logs and counts a task callable's exception, caught at the worker
    // boundary. `handle` is TaskHandle{} for the inline-execution path, which
    // has no pool slot to name.
    void record_uncaught_exception(TaskHandle handle, const char* what) noexcept;

    [[nodiscard]] bool is_complete_internal(TaskHandle handle) const;

    // Registers task_index as awaited by the calling thread's handle-wait.
    // Returns the claimed slot, or kMaxHandleWaiterSlots if none was free (in
    // which case the overflow counter was incremented instead).
    [[nodiscard]] std::size_t register_handle_waiter(std::uint32_t task_index) noexcept;
    void                      unregister_handle_waiter(std::size_t slot) noexcept;

    RuntimeConfig config_;
    TaskPool      pool_;

    mutable std::mutex      completion_mutex_;
    std::condition_variable completion_cv_;

    // Waiters are counted SEPARATELY by what they are waiting for, and the
    // distinction is worth several microseconds per task.
    //
    // A single counter meant that one thread sitting in wait_all() made every
    // completion take the mutex and call notify_all() - waking that thread once
    // per task to re-check a predicate that stays false until the very last one.
    // Measured at 65536 tasks, that turned a ~200 ns dispatch into ~5 us and made
    // the runtime 20x slower than running the work serially.
    //
    // A wait_all() waiter can only be satisfied when outstanding_ reaches zero, so
    // it is notified then and not before. Per-handle waiters used to need a
    // notification on EVERY completion in the runtime, on the theory that path is
    // rare - inside a worker, wait(handle) helps rather than blocking. It is not
    // rare: it is exactly how the simulation's frame barrier waits from the main
    // thread, three times a tick, and the assumption cost every one of the ~150
    // tasks in a frame a mutex lock and a notify_all while any one of those three
    // waits was outstanding. handle_waiter_slots_ below fixes the same class of
    // bug wait_all() already fixed once, applied here too.
    std::atomic<std::uint32_t> handle_waiters_{0};
    std::atomic<std::uint32_t> all_waiters_{0};

    // Fixed-capacity set of task-pool indices an external-thread handle-waiter is
    // currently blocked on. complete() scans this so it can wake only a waiter
    // whose OWN task just finished, instead of every waiter on every completion.
    //
    // Bounded rather than growable, so wait() never touches the allocator.
    // Overflow (more concurrent handle-waiters than slots) falls back to the old
    // wake-on-any-completion behaviour for as long as any overflowed waiter is
    // still around - correctness over precision, the same trade pool exhaustion
    // makes by degrading to inline execution rather than failing.
    //
    // Reference-counted rather than a bool: a bool cleared by the last waiter to
    // leave can race with a different waiter concurrently becoming the reason the
    // fallback is still needed, clearing it while still required. A counter, like
    // handle_waiters_ and all_waiters_ above, has no such window.
    static constexpr std::uint32_t kNoWaitedIndex        = 0xFFFFFFFFu;
    static constexpr std::size_t   kMaxHandleWaiterSlots = 32;

    struct HandleWaiterSlot {
        std::atomic<std::uint32_t> task_index{kNoWaitedIndex};
    };
    HandleWaiterSlot            handle_waiter_slots_[kMaxHandleWaiterSlots];
    std::atomic<std::uint32_t>  handle_waiter_overflow_count_{0};

    // NOT sharded, and on its own cache line.
    //
    // wait_all() needs an exact zero from this, and summing sixteen shards that
    // other cores are writing would mean sixteen cache misses per check - worse
    // than the single atomic. So it stays one counter and instead gets isolated:
    // it previously shared a line with the four counters below, all of which are
    // also written per task, so the line was contended for several independent
    // reasons at once.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> outstanding_{0};

    // Sharded. Written on the hot path, read only by reports and tests.
    alignas(kCacheLineSize) ShardedCounter completed_;
    ShardedCounter inline_executions_;
    ShardedCounter dependency_edges_;
    ShardedCounter dependencies_pre_satisfied_;
    ShardedCounter uncaught_exceptions_;
};

TB_END_CACHE_ALIGNED_TYPE

} // namespace thunderbolt
