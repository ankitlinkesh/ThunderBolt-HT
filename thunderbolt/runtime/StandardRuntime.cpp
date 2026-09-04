#include <thunderbolt/runtime/StandardRuntime.hpp>

#include <cassert>
#include <utility>

namespace thunderbolt {
namespace {

// Identifies the runtime a thread is working for, and which worker it is.
// Keyed on the runtime instance so that nesting runtimes stays unambiguous.
struct WorkerIdentity {
    const void*   owner = nullptr;
    std::uint32_t index = TaskContext::kExternalThread;
};

thread_local WorkerIdentity t_identity;

} // namespace

StandardRuntime::StandardRuntime(RuntimeConfig config)
    : config_(config), pool_(config.task_capacity) {
    std::uint32_t requested = config.worker_count;
    if (requested == 0) {
        requested = std::thread::hardware_concurrency();
        if (requested == 0) {
            requested = 1;  // hardware_concurrency is allowed to fail
        }
    }
    worker_count_ = requested;

    running_.store(true, std::memory_order_release);

    workers_.reserve(worker_count_);
    for (std::uint32_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
    }
}

StandardRuntime::~StandardRuntime() {
    // Drain before stopping. Destroying a runtime with work still queued would
    // silently drop tasks, and a dropped task is indistinguishable from a
    // scheduling bug when it shows up later as a wrong result.
    wait_all();

    running_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(queue_mutex_);
        queue_cv_.notify_all();
    }

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool StandardRuntime::on_own_worker(std::uint32_t& out_index) const {
    if (t_identity.owner == this) {
        out_index = t_identity.index;
        return true;
    }
    return false;
}

TaskHandle StandardRuntime::submit(TaskDesc desc) {
    const TaskPriority priority = desc.priority;

    // Count the task as outstanding BEFORE it becomes visible to a worker.
    // Doing it after would let a worker complete the task and decrement a
    // counter that had not yet been incremented.
    outstanding_.fetch_add(1, std::memory_order_relaxed);

    TaskHandle handle = pool_.acquire(std::move(desc));

    if (!handle.valid()) {
        // Pool exhausted. Degrade to inline execution rather than failing or
        // reaching for the allocator: the work still happens and stays correct,
        // the parallelism is reduced, and the counter makes that visible in
        // results instead of hiding it.
        inline_executions_.fetch_add(1, std::memory_order_relaxed);

        std::uint32_t worker_index = TaskContext::kExternalThread;
        (void)on_own_worker(worker_index);  // leaves kExternalThread when not a worker
        TaskContext ctx{this, worker_index};

        // desc was moved-from by acquire() only on the success path; on failure
        // it still owns the body.
        TaskDesc local = std::move(desc);
        if (local.function) {
            local.function(ctx);
        }

        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        return TaskHandle{};
    }

    Task* task = pool_.get(handle);
    assert(task != nullptr);
    task->state.store(TaskState::Queued, std::memory_order_release);

    {
        std::lock_guard lock(queue_mutex_);
        ready_[static_cast<std::size_t>(priority)].push_back(handle);
        ++queued_count_;
    }
    // notify_one, not notify_all: waking every worker for a single task produces
    // a thundering herd that contends on the queue lock and then goes back to
    // sleep.
    queue_cv_.notify_one();

    return handle;
}

TaskHandle StandardRuntime::pop_locked() {
    if (queued_count_ == 0) {
        return TaskHandle{};
    }
    // Scan highest priority first (S13). FIFO within a priority, which is what
    // stops a steady stream of same-priority work from reordering arbitrarily.
    for (std::size_t p = 0; p < kPriorityCount; ++p) {
        std::deque<TaskHandle>& q = ready_[p];
        if (!q.empty()) {
            TaskHandle handle = q.front();
            q.pop_front();
            --queued_count_;
            return handle;
        }
    }
    return TaskHandle{};
}

TaskHandle StandardRuntime::try_pop() {
    std::lock_guard lock(queue_mutex_);
    return pop_locked();
}

void StandardRuntime::execute(TaskHandle handle, TaskContext& ctx) {
    Task* task = pool_.get(handle);
    if (task == nullptr) {
        // Cannot happen: a queued handle is not released until completion.
        assert(false && "queued handle resolved to a recycled slot");
        return;
    }

    task->state.store(TaskState::Executing, std::memory_order_release);
    task->function(ctx);
    complete(handle);
}

void StandardRuntime::complete(TaskHandle handle) {
    Task* task = pool_.get(handle);
    assert(task != nullptr);
    // seq_cst, not release: this store is one half of the Store-Load pair
    // described below.
    task->state.store(TaskState::Completed, std::memory_order_seq_cst);

    // Releasing the slot bumps its generation, which is what turns any handle
    // still held by a waiter into a stale - and therefore "complete" - handle.
    pool_.release(handle);

    outstanding_.fetch_sub(1, std::memory_order_acq_rel);

    // Fast path: with nobody waiting there is no wakeup to deliver, so the
    // completion path never touches the completion mutex. Under load that is
    // almost every completion, and it is what keeps this baseline honest rather
    // than artificially slow (S11).
    //
    // The fence is load-bearing, not defensive. Skipping the notification is
    // only safe if this thread cannot observe waiters_ == 0 while a waiter
    // simultaneously fails to observe our completion. Publishing the state and
    // then reading waiters_ is a Store-Load pair, which is exactly the ordering
    // x86 is permitted to reverse - and which ARM64 reverses far more freely.
    // A seq_cst fence on this side, paired with the seq_cst increment on the
    // waiter's side, is what rules out the interleaving where each thread misses
    // the other and the waiter sleeps forever.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (waiters_.load(std::memory_order_relaxed) == 0) {
        return;
    }

    // Briefly taking the mutex closes the remaining lost-wakeup window: a waiter
    // is either not yet holding it (and will re-evaluate its predicate after the
    // store above), or already inside wait() (and will receive the notification).
    {
        std::lock_guard lock(completion_mutex_);
    }
    completion_cv_.notify_all();
}

bool StandardRuntime::try_execute_one(TaskContext& ctx) {
    TaskHandle handle = try_pop();
    if (!handle.valid()) {
        return false;
    }
    execute(handle, ctx);
    return true;
}

void StandardRuntime::worker_loop(std::uint32_t worker_index) {
    t_identity.owner = this;
    t_identity.index = worker_index;

    TaskContext ctx{this, worker_index};

    while (running_.load(std::memory_order_acquire)) {
        TaskHandle handle;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return queued_count_ > 0 || !running_.load(std::memory_order_acquire);
            });

            if (queued_count_ == 0) {
                // Woken for shutdown.
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            handle = pop_locked();
        }

        if (handle.valid()) {
            execute(handle, ctx);
        }
    }

    t_identity.owner = nullptr;
    t_identity.index = TaskContext::kExternalThread;
}

bool StandardRuntime::is_complete_unlocked(TaskHandle handle) const {
    if (!handle.valid()) {
        return true;  // never submitted, or ran inline: nothing to wait for
    }
    const Task* task = pool_.get(handle);
    if (task == nullptr) {
        return true;  // stale handle: the task completed and its slot was reused
    }

    const TaskState state = task->state.load(std::memory_order_acquire);

    // Re-validate the generation AFTER reading the state, not just before.
    //
    // Between get() resolving the handle and the load above, our task can
    // complete, release its slot, and have that slot handed to a freshly
    // submitted task - at which point `state` describes a stranger. Reporting
    // that as "not complete" is not merely imprecise: the waiter would then sleep
    // until the UNRELATED task finishes, because its own task will never signal
    // again. A generation that has moved on means our task is done, whatever the
    // slot currently says.
    if (task->generation.load(std::memory_order_acquire) != handle.generation) {
        return true;
    }

    return state == TaskState::Completed;
}

bool StandardRuntime::is_complete(TaskHandle handle) const { return is_complete_unlocked(handle); }

void StandardRuntime::wait(TaskHandle handle) {
    if (is_complete_unlocked(handle)) {
        return;
    }

    std::uint32_t worker_index = TaskContext::kExternalThread;
    if (on_own_worker(worker_index)) {
        // Help-on-wait. Parking the worker here would deadlock as soon as every
        // worker is inside a wait(), because the tasks they are waiting for would
        // have nobody left to run them.
        TaskContext ctx{this, worker_index};
        while (!is_complete_unlocked(handle)) {
            if (!try_execute_one(ctx)) {
                // Nothing runnable and our task is still outstanding, so another
                // worker is executing it. Yield rather than spin.
                std::this_thread::yield();
            }
        }
        return;
    }

    // External thread: block rather than help.
    //
    // Helping here would be faster, and is deliberately not done. S58's scaling
    // sweep interprets worker_count as the number of threads executing tasks; a
    // main thread that also ran tasks would make "1 worker" mean two executors
    // and quietly inflate every low-worker-count measurement.
    // seq_cst: pairs with the fence in complete(). See the comment there.
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    {
        std::unique_lock lock(completion_mutex_);
        completion_cv_.wait(lock, [this, handle] { return is_complete_unlocked(handle); });
    }
    waiters_.fetch_sub(1, std::memory_order_seq_cst);
}

void StandardRuntime::wait_all() {
    // Note the scope: this waits for the runtime to be IDLE, not for "the work I
    // submitted". Tasks submitted by other threads after this call are included,
    // so a worker calling wait_all() can be held for as long as anyone keeps
    // submitting. That is intended - it is what makes wait_all() usable as a
    // frame barrier - but it is not the "wait for my subtree" that callers
    // sometimes assume. Phase D's dependency edges are the tool for that.
    if (outstanding_.load(std::memory_order_acquire) == 0) {
        return;
    }

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
