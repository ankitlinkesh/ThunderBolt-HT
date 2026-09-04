#include <thunderbolt/runtime/RuntimeBase.hpp>

#include <thunderbolt/cpu/topology/CpuTopology.hpp>

#include <cassert>
#include <thread>
#include <utility>
#include <vector>

namespace thunderbolt {
namespace {

// Depth of nested wait() calls on one thread. Help-on-wait means a waiting worker
// runs other tasks, which may themselves wait - so the stack can grow without any
// single call site looking recursive. Unbounded growth is a stack overflow with a
// confusing trace, so debug builds fail loudly at a depth no reasonable frame
// graph reaches (S62).
thread_local int t_wait_depth = 0;

constexpr int kMaxWaitDepth = 256;

struct WaitDepthGuard {
    WaitDepthGuard() {
        ++t_wait_depth;
        assert(t_wait_depth < kMaxWaitDepth &&
               "wait() nested beyond the supported depth: help-on-wait let a worker recurse into "
               "tasks that themselves wait. Restructure with submit_after() dependencies rather "
               "than nested waits.");
    }
    ~WaitDepthGuard() { --t_wait_depth; }
};

} // namespace

RuntimeBase::RuntimeBase(RuntimeConfig config) : config_(config), pool_(config.task_capacity) {}

std::uint32_t RuntimeBase::resolve_worker_count(const RuntimeConfig& config) {
    if (config.worker_count != 0) {
        return config.worker_count;
    }
    const std::uint32_t logical = cpu_topology().logical_processor_count;
    return logical > 0 ? logical : 1;
}

TaskHandle RuntimeBase::submit(TaskDesc desc) {
    const TaskPriority priority = desc.priority;

    // Counted as outstanding BEFORE the task can become visible to a worker. The
    // other order lets a worker complete the task and decrement a counter that
    // has not been incremented yet.
    outstanding_.fetch_add(1, std::memory_order_relaxed);

    TaskHandle handle = pool_.acquire(std::move(desc));
    if (!handle.valid()) {
        // Pool exhausted. Degrade to inline execution rather than failing or
        // reaching for the allocator: the work still happens and stays correct,
        // only the parallelism suffers, and the counter makes that visible.
        run_inline(std::move(desc));
        return TaskHandle{};
    }

    Task* task = pool_.get(handle);
    assert(task != nullptr);
    task->state.store(TaskState::Queued, std::memory_order_release);

    enqueue_ready(handle, priority);
    return handle;
}

TaskHandle RuntimeBase::submit_after(TaskDesc desc, const TaskHandle* dependencies,
                                     std::size_t dependency_count) {
    if (dependency_count == 0) {
        return submit(std::move(desc));
    }

    const TaskPriority priority = desc.priority;

    outstanding_.fetch_add(1, std::memory_order_relaxed);

    TaskHandle handle = pool_.acquire(std::move(desc));
    if (!handle.valid()) {
        // Pool exhausted, but this task has dependencies, so it cannot simply run
        // now: doing so would violate the ordering the caller asked for. Wait for
        // the predecessors first, then run inline. Safe from a worker because
        // wait() helps rather than parking.
        for (std::size_t i = 0; i < dependency_count; ++i) {
            wait(dependencies[i]);
        }
        run_inline(std::move(desc));
        return TaskHandle{};
    }

    Task* task = pool_.get(handle);
    assert(task != nullptr);
    task->state.store(TaskState::Waiting, std::memory_order_release);

    // The +1 is a guard, not an edge.
    //
    // Without it, a predecessor completing while this loop is still running could
    // drive the counter to zero and enqueue the task before its remaining edges
    // have even been counted - and the task would then run before dependencies it
    // was about to declare. The guard is removed only after every edge is
    // registered, so the count is never transiently low.
    task->pending_dependencies.store(static_cast<std::uint32_t>(dependency_count) + 1,
                                     std::memory_order_relaxed);

    for (std::size_t i = 0; i < dependency_count; ++i) {
        const TaskHandle dependency = dependencies[i];
        dependency_edges_.fetch_add(1, std::memory_order_relaxed);

        Task* predecessor = pool_.get(dependency);

        // A null lookup means the handle is stale: that task finished and its slot
        // was recycled. Already satisfied.
        const bool registered =
            (predecessor != nullptr) &&
            predecessor->try_add_successor(handle, dependency.generation);

        if (!registered) {
            dependencies_pre_satisfied_.fetch_add(1, std::memory_order_relaxed);
            task->pending_dependencies.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // Remove the guard. If this is the transition to zero, every dependency was
    // already satisfied and nobody else will enqueue the task.
    if (task->pending_dependencies.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        task->state.store(TaskState::Queued, std::memory_order_release);
        enqueue_ready(handle, priority);
    }

    return handle;
}

void RuntimeBase::run_inline(TaskDesc&& desc) {
    inline_executions_.fetch_add(1, std::memory_order_relaxed);

    std::uint32_t worker_index = TaskContext::kExternalThread;
    (void)on_own_worker(worker_index);
    TaskContext ctx{this, worker_index};

    TaskDesc local = std::move(desc);
    if (local.function) {
        local.function(ctx);
    }

    // Balances the increment performed before the failed acquire.
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

void RuntimeBase::execute(TaskHandle handle, TaskContext& ctx) {
    Task* task = pool_.get(handle);
    if (task == nullptr) {
        // A claimed handle is not released until completion, so this is
        // unreachable unless the derived runtime handed out the same task twice.
        assert(false && "claimed handle resolved to a recycled slot");
        return;
    }

    // Detects a task being dispatched twice (S62 "double completion"). In debug
    // this is an exchange rather than a store, so the previous state is checked
    // rather than silently overwritten.
#if THUNDERBOLT_DEBUG
    const TaskState previous = task->state.exchange(TaskState::Executing, std::memory_order_acq_rel);
    assert((previous == TaskState::Queued || previous == TaskState::Ready) &&
           "task dispatched twice, or dispatched from an unexpected state");
    (void)previous;
#else
    task->state.store(TaskState::Executing, std::memory_order_release);
#endif

    task->function(ctx);
    complete(handle);
}

void RuntimeBase::release_dependent(TaskHandle dependent) {
    Task* task = pool_.get(dependent);
    if (task == nullptr) {
        // The dependent was recycled, which can only happen if it already ran.
        // Nothing to release.
        return;
    }

    if (task->pending_dependencies.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // We drove it to zero, so we - and only we - enqueue it.
        task->state.store(TaskState::Queued, std::memory_order_release);
        enqueue_ready(dependent, task->priority);
    }
}

void RuntimeBase::complete(TaskHandle handle) {
    Task* task = pool_.get(handle);
    assert(task != nullptr);

    // Close the successor list and take it BEFORE releasing the slot. Any
    // registration that got in first is therefore seen here, and any that arrives
    // later finds the list closed and accounts for the dependency itself. That
    // hand-off is what makes "predecessor finishes while a dependent is still
    // registering" safe in both directions.
    //
    // thread_local rather than a stack vector: completion is the hottest path in
    // the runtime, and this reuses one allocation per worker instead of one per
    // completed task.
    static thread_local std::vector<TaskHandle> successors;
    task->take_successors(successors);

    // seq_cst rather than release: this store is one half of the Store-Load pair
    // discussed below.
    task->state.store(TaskState::Completed, std::memory_order_seq_cst);

    // Releasing the slot advances its generation, which is what turns a handle
    // still held by a waiter into a stale - therefore complete - handle.
    pool_.release(handle);

    // Dependents are released after this task is observably complete, so a
    // dependent that immediately inspects its predecessor's handle sees a
    // finished task rather than a racing one.
    for (TaskHandle dependent : successors) {
        release_dependent(dependent);
    }
    successors.clear();

    completed_.fetch_add(1, std::memory_order_relaxed);
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);

    // Fast path: with nobody waiting there is no wakeup to deliver, so completion
    // never touches the completion mutex.
    //
    // The fence is load-bearing. Skipping the notification is safe only if this
    // thread cannot read waiters_ == 0 while a waiter simultaneously fails to
    // observe our completion. Publishing state and then reading waiters_ is a
    // Store-Load pair - the one ordering x86 may reverse, and which ARM64
    // reverses freely. This fence, paired with the seq_cst increment on the
    // waiter's side, rules out the interleaving where both miss each other and
    // the waiter never wakes.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (waiters_.load(std::memory_order_relaxed) == 0) {
        return;
    }

    // Taking the mutex briefly closes the remaining window: a waiter is either
    // not yet holding it (and will re-evaluate its predicate after the store
    // above) or already inside wait() (and will receive the notification).
    {
        std::lock_guard lock(completion_mutex_);
    }
    completion_cv_.notify_all();
}

bool RuntimeBase::is_complete_internal(TaskHandle handle) const {
    if (!handle.valid()) {
        return true;  // never submitted, or ran inline: nothing to wait for
    }

    const Task* task = pool_.get(handle);
    if (task == nullptr) {
        return true;  // stale: the task completed and its slot was reused
    }

    const TaskState state = task->state.load(std::memory_order_acquire);

    // Re-validate the generation AFTER reading state, not only before.
    //
    // Between get() resolving the handle and the load above, our task can
    // complete, release its slot, and have that slot handed to a newly submitted
    // task - at which point `state` describes a stranger. Reporting that as "not
    // complete" is not just imprecise: the waiter would then sleep until the
    // UNRELATED task finishes, because its own will never signal again.
    if (task->generation.load(std::memory_order_acquire) != handle.generation) {
        return true;
    }

    return state == TaskState::Completed;
}

bool RuntimeBase::is_complete(TaskHandle handle) const { return is_complete_internal(handle); }

void RuntimeBase::wait(TaskHandle handle) {
    if (is_complete_internal(handle)) {
        return;
    }

    WaitDepthGuard depth_guard;

    std::uint32_t worker_index = TaskContext::kExternalThread;
    if (on_own_worker(worker_index)) {
        // Help-on-wait. Parking the worker here would deadlock as soon as every
        // worker sat inside a wait(), because the tasks they wait for would have
        // nobody left to run them. The alternative - a stackful fiber per task -
        // is a deliberate deferral, not an oversight.
        TaskContext ctx{this, worker_index};
        while (!is_complete_internal(handle)) {
            if (!try_execute_one(ctx)) {
                // Nothing runnable, but our task is still outstanding, so another
                // worker holds it. Yield rather than spin.
                std::this_thread::yield();
            }
        }
        return;
    }

    // External thread: block rather than help.
    //
    // Helping here would be faster and is deliberately not done. S58 reads
    // worker_count as the number of threads executing tasks; a main thread that
    // also ran tasks would make "1 worker" mean two executors and inflate every
    // low-worker-count measurement in the scaling sweep.
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    {
        std::unique_lock lock(completion_mutex_);
        completion_cv_.wait(lock, [this, handle] { return is_complete_internal(handle); });
    }
    waiters_.fetch_sub(1, std::memory_order_seq_cst);
}

void RuntimeBase::wait_all() {
    // Note the scope: this waits for the runtime to be IDLE, not for "the work I
    // submitted". Tasks submitted by other threads after this call are included,
    // so a worker calling wait_all() can be held for as long as anyone keeps
    // submitting. Intended - it is what makes wait_all() usable as a frame
    // barrier - but not the "wait for my subtree" callers sometimes assume.
    if (outstanding_.load(std::memory_order_acquire) == 0) {
        return;
    }

    WaitDepthGuard depth_guard;

    std::uint32_t worker_index = TaskContext::kExternalThread;
    if (on_own_worker(worker_index)) {
        TaskContext ctx{this, worker_index};
        while (outstanding_.load(std::memory_order_acquire) != 0) {
            if (!try_execute_one(ctx)) {
                std::this_thread::yield();
            }
        }
        return;
    }

    waiters_.fetch_add(1, std::memory_order_seq_cst);
    {
        std::unique_lock lock(completion_mutex_);
        completion_cv_.wait(lock,
                            [this] { return outstanding_.load(std::memory_order_acquire) == 0; });
    }
    waiters_.fetch_sub(1, std::memory_order_seq_cst);
}

} // namespace thunderbolt
