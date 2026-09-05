#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#include <thunderbolt/cpu/affinity/Affinity.hpp>

#include <cassert>
#include <utility>

namespace thunderbolt {
namespace {

struct WorkerIdentity {
    const void*   owner = nullptr;
    std::uint32_t index = TaskContext::kExternalThread;
};

thread_local WorkerIdentity t_identity;

// Spin attempts before a worker parks. Chosen to cover the gap between two
// dependent tasks in a frame graph without burning a measurable slice of a 15 W
// power budget; Phase G revisits it with data rather than intuition.
constexpr int kSpinsBeforePark = 64;

// Tasks pulled from the global queue in one go. Large enough that the global lock
// is amortised, small enough that one worker cannot hoard an entire injection
// burst and leave the others idle.
constexpr int kGlobalDrainBatch = 32;

inline std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

} // namespace

ThunderboltRuntime::ThunderboltRuntime(RuntimeConfig config) : RuntimeBase(config) {
    worker_count_ = resolve_worker_count(config);

    // Per worker, per priority. Overflow is handled - it spills to the global
    // queue and is counted - so this is a tuning parameter, not a correctness
    // limit.
    const std::size_t per_queue_capacity = (config.deque_capacity > 0) ? config.deque_capacity : 1024;

    workers_.reserve(worker_count_);
    for (std::uint32_t i = 0; i < worker_count_; ++i) {
        auto worker = std::make_unique<WorkerState>();
        for (std::size_t p = 0; p < kPriorityCount; ++p) {
            worker->queues[p] = std::make_unique<AbpDeque<TaskHandle>>(per_queue_capacity);
        }
        // Seeded per worker and never zero - xorshift is absorbing at zero, which
        // would make one worker always pick the same victim.
        worker->rng = 0x9E3779B97F4A7C15ull * (static_cast<std::uint64_t>(i) + 1);
        workers_.push_back(std::move(worker));
    }

    running_.store(true, std::memory_order_release);

    threads_.reserve(worker_count_);
    for (std::uint32_t i = 0; i < worker_count_; ++i) {
        threads_.emplace_back([this, i] { worker_loop(i); });
    }
}

ThunderboltRuntime::~ThunderboltRuntime() {
    // Drain before stopping, so shutdown never silently discards queued work.
    wait_all();

    running_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(sleep_mutex_);
        sleep_cv_.notify_all();
    }

    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool ThunderboltRuntime::on_own_worker(std::uint32_t& out_index) const {
    if (t_identity.owner == this) {
        out_index = t_identity.index;
        return true;
    }
    return false;
}

void ThunderboltRuntime::enqueue_ready(TaskHandle handle, TaskPriority priority) {
    if (config().scheduler == SchedulerMode::Static) {
        // The SUBMITTER picks the worker, round-robin, and it stays there. No
        // stealing will move it later - that is the whole point of the mode, and
        // what makes it the control for "does stealing actually help?".
        const std::uint32_t target =
            static_cursor_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
        WorkerState& worker = *workers_[target];
        {
            std::lock_guard lock(worker.inbox_mutex);
            worker.inbox[static_cast<std::size_t>(priority)].push_back(handle);
            ++worker.inbox_count;
        }
        submissions_global_.fetch_add(1, std::memory_order_relaxed);
        wake_one_worker();
        return;
    }

    std::uint32_t worker_index = TaskContext::kExternalThread;
    bool          queued_local = false;

    if (on_own_worker(worker_index)) {
        // Queued from inside a task: push onto the submitting worker's own deque.
        // This is the locality that makes work stealing worth having - a task's
        // children stay on the core that produced them, and only migrate when
        // another worker would otherwise sit idle.
        WorkerState& worker = *workers_[worker_index];
        queued_local = worker.queues[static_cast<std::size_t>(priority)]->push(handle);

        if (!queued_local) {
            // Bounded deque full. Spilling to the global queue keeps the task
            // running somewhere rather than blocking or growing the deque; the
            // counter says how often the capacity was too small.
            local_overflows_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (queued_local) {
        submissions_local_.fetch_add(1, std::memory_order_relaxed);
    } else {
        submissions_global_.fetch_add(1, std::memory_order_relaxed);
        global_.push(handle, priority);
    }

    wake_one_worker();
}

void ThunderboltRuntime::wake_one_worker() {
    // The work is already visible at this point. Reading sleeping_ after
    // publishing it is a Store-Load pair, so it needs the same seq_cst fence as
    // the completion path in RuntimeBase: without it, this thread can read
    // "nobody is asleep" while a worker simultaneously fails to see the work and
    // goes to sleep anyway.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_relaxed) == 0) {
        return;  // fast path: everyone is busy, no wakeup needed
    }

    {
        std::lock_guard lock(sleep_mutex_);
    }
    sleep_cv_.notify_one();
}

TaskHandle ThunderboltRuntime::pop_local(WorkerState& worker) {
    // Aging inverts the scan on one pop in kAgingInterval, so low-priority work
    // gets a guaranteed share instead of only whatever is left over. Strict
    // priority - the default - never takes this branch.
    const bool lowest_first =
        aging_pop_favours_lowest(config().scheduler, worker.pop_index);

    for (std::size_t i = 0; i < kPriorityCount; ++i) {
        const std::size_t p = lowest_first ? (kPriorityCount - 1 - i) : i;
        TaskHandle        handle;
        if (worker.queues[p]->pop(handle)) {
            ++worker.pop_index;
            return handle;
        }
    }
    return TaskHandle{};
}

TaskHandle ThunderboltRuntime::pop_inbox(WorkerState& worker) {
    std::lock_guard lock(worker.inbox_mutex);
    if (worker.inbox_count == 0) {
        return TaskHandle{};
    }
    const bool lowest_first =
        aging_pop_favours_lowest(config().scheduler, worker.pop_index);

    for (std::size_t i = 0; i < kPriorityCount; ++i) {
        const std::size_t p = lowest_first ? (kPriorityCount - 1 - i) : i;
        if (!worker.inbox[p].empty()) {
            TaskHandle handle = worker.inbox[p].front();
            worker.inbox[p].pop_front();
            --worker.inbox_count;
            ++worker.pop_index;
            return handle;
        }
    }
    return TaskHandle{};
}

TaskHandle ThunderboltRuntime::drain_global(WorkerState& worker) {
    // Aging applies here too. Externally submitted tasks never touch a worker
    // deque, so rotating only the deque scan would leave them strictly
    // prioritised - and the starvation test caught exactly that.
    const bool lowest_first =
        aging_pop_favours_lowest(config().scheduler, worker.pop_index);

    TaskHandle first = global_.pop(lowest_first);
    if (!first.valid()) {
        return TaskHandle{};
    }

    ++worker.pop_index;
    worker.global_pops.fetch_add(1, std::memory_order_relaxed);

    // Pull a few more into the local deque while we hold the cache line warm.
    // These become stealable, so a burst submitted from outside still spreads
    // across workers rather than serialising on the global lock.
    for (int i = 1; i < kGlobalDrainBatch; ++i) {
        TaskHandle extra = global_.pop();
        if (!extra.valid()) {
            break;
        }
        // Safe to dereference: we popped this handle, so no other thread has
        // claimed it and nothing can have completed or recycled it yet.
        const Task* task = pool().get(extra);
        assert(task != nullptr && "handle popped from the global queue must resolve");
        const TaskPriority priority = task->priority;

        if (!worker.queues[static_cast<std::size_t>(priority)]->push(extra)) {
            global_.push(extra, priority);  // local deque full; put it back
            break;
        }
    }

    return first;
}

TaskHandle ThunderboltRuntime::steal_from_others(WorkerState& worker,
                                                 std::uint32_t worker_index) {
    if (worker_count_ <= 1) {
        return TaskHandle{};
    }

    worker.steal_attempts.fetch_add(1, std::memory_order_relaxed);

    // Randomised victim selection, as the Blumofe-Leiserson bound assumes.
    // Deterministic scanning (always try worker 0 first) makes early workers hot
    // and skews which deque is contended, which shows up as a scheduling result
    // that is really an artefact of the victim policy.
    const std::uint32_t start = static_cast<std::uint32_t>(xorshift64(worker.rng) % worker_count_);

    for (std::uint32_t offset = 0; offset < worker_count_; ++offset) {
        const std::uint32_t victim_index = (start + offset) % worker_count_;
        if (victim_index == worker_index) {
            continue;
        }

        WorkerState& victim = *workers_[victim_index];
        for (std::size_t p = 0; p < kPriorityCount; ++p) {
            TaskHandle handle;
            const StealOutcome outcome = victim.queues[p]->steal(handle);
            if (outcome == StealOutcome::Success) {
                worker.steals_succeeded.fetch_add(1, std::memory_order_relaxed);
                return handle;
            }
            // Abort means we lost a race, which implies work IS there. Retrying
            // the same victim immediately would just lose again to the thread
            // that beat us, so fall through to the next one.
        }
    }

    worker.steals_failed.fetch_add(1, std::memory_order_relaxed);
    return TaskHandle{};
}

TaskHandle ThunderboltRuntime::acquire_next(WorkerState& worker, std::uint32_t worker_index) {
    // Order: local, then global, then steal.
    //
    // S10's sketch puts stealing before the global queue. Deviating deliberately:
    // with steal-before-global, externally submitted work is only picked up once
    // every worker has drained AND failed to steal, so a workload whose tasks
    // spawn their own children can starve the injection queue indefinitely.
    // Checking global first costs one relaxed atomic load when it is empty, which
    // is the common case, and stealing stays the last resort - which is also the
    // right order by cost, since a steal touches another core's cache line.
    if (config().scheduler == SchedulerMode::Static) {
        // Own inbox only. No global drain, no stealing: a static schedule that
        // quietly rebalanced would not be a static schedule, and the comparison
        // against stealing would measure nothing.
        return pop_inbox(worker);
    }

    // On an aging turn, look for the LOWEST-priority work available anywhere
    // before anything else, and check the global queue first.
    //
    // Checking only the local deque is not enough, and the starvation test proved
    // it twice. Externally submitted tasks land in the global queue; worse, the
    // batch drain pulls 32 high-priority tasks into the local deque at a time, so
    // a local-only rotation keeps finding nothing but criticals while the
    // background task it is supposed to rescue is still sitting in the queue it
    // never looks at.
    if (aging_pop_favours_lowest(config().scheduler, worker.pop_index)) {
        if (TaskHandle handle = global_.pop(/*lowest_first=*/true); handle.valid()) {
            ++worker.pop_index;
            worker.global_pops.fetch_add(1, std::memory_order_relaxed);
            return handle;
        }
    }

    if (TaskHandle handle = pop_local(worker); handle.valid()) {
        return handle;
    }
    if (TaskHandle handle = drain_global(worker); handle.valid()) {
        return handle;
    }
    return steal_from_others(worker, worker_index);
}

bool ThunderboltRuntime::any_work_available() const {
    if (global_.size_hint() > 0) {
        return true;
    }
    if (config().scheduler == SchedulerMode::Static) {
        for (const auto& worker : workers_) {
            std::lock_guard lock(worker->inbox_mutex);
            if (worker->inbox_count > 0) {
                return true;
            }
        }
        return false;
    }
    for (const auto& worker : workers_) {
        for (std::size_t p = 0; p < kPriorityCount; ++p) {
            if (!worker->queues[p]->empty_hint()) {
                return true;
            }
        }
    }
    return false;
}

void ThunderboltRuntime::park_worker(WorkerState& worker) {
    std::unique_lock lock(sleep_mutex_);

    // Register as sleeping BEFORE the final check, not after.
    //
    // The other order loses wakeups: a submitter could publish work and read
    // sleeping_ == 0 in the window between this thread's check and its
    // increment, decide no notification is needed, and leave this worker asleep
    // with runnable work in the system. Incrementing first means a submitter
    // either sees the increment (and must take this mutex to notify, which it
    // cannot do until we are inside wait) or publishes before our check (and we
    // see the work and never sleep).
    sleeping_.fetch_add(1, std::memory_order_seq_cst);

    if (!running_.load(std::memory_order_acquire) || any_work_available()) {
        sleeping_.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }

    worker.parks.fetch_add(1, std::memory_order_relaxed);
    sleep_cv_.wait(lock);
    sleeping_.fetch_sub(1, std::memory_order_seq_cst);
}

bool ThunderboltRuntime::try_execute_one(TaskContext& ctx) {
    // Used by help-on-wait, which may run on any worker of this runtime.
    std::uint32_t worker_index = TaskContext::kExternalThread;
    if (!on_own_worker(worker_index)) {
        return false;
    }

    WorkerState& worker = *workers_[worker_index];
    TaskHandle   handle = acquire_next(worker, worker_index);
    if (!handle.valid()) {
        return false;
    }

    worker.executed.fetch_add(1, std::memory_order_relaxed);
    execute(handle, ctx);
    return true;
}

void ThunderboltRuntime::worker_loop(std::uint32_t worker_index) {
    t_identity.owner = this;
    t_identity.index = worker_index;

    if (apply_worker_affinity(worker_index, config().affinity)) {
        pinned_workers_.fetch_add(1, std::memory_order_relaxed);
    }

    WorkerState& worker = *workers_[worker_index];
    TaskContext  ctx{this, worker_index};

    int idle_spins = 0;

    while (running_.load(std::memory_order_acquire)) {
        TaskHandle handle = acquire_next(worker, worker_index);

        if (handle.valid()) {
            idle_spins = 0;
            worker.executed.fetch_add(1, std::memory_order_relaxed);
            execute(handle, ctx);
            continue;
        }

        // Nothing anywhere. Spin briefly before parking: in a frame graph the
        // next task usually appears within microseconds, and a condition-variable
        // round trip costs more than the spin does.
        if (++idle_spins < kSpinsBeforePark) {
            std::this_thread::yield();
            continue;
        }

        park_worker(worker);
        idle_spins = 0;
    }

    t_identity.owner = nullptr;
    t_identity.index = TaskContext::kExternalThread;
}

ThunderboltStats ThunderboltRuntime::stats() const {
    ThunderboltStats out;
    for (const auto& worker : workers_) {
        out.tasks_executed += worker->executed.load(std::memory_order_relaxed);
        out.steal_attempts += worker->steal_attempts.load(std::memory_order_relaxed);
        out.steals_succeeded += worker->steals_succeeded.load(std::memory_order_relaxed);
        out.steals_failed += worker->steals_failed.load(std::memory_order_relaxed);
        out.global_pops += worker->global_pops.load(std::memory_order_relaxed);
        out.worker_parks += worker->parks.load(std::memory_order_relaxed);
    }
    out.local_overflows    = local_overflows_.load(std::memory_order_relaxed);
    out.submissions_local  = submissions_local_.load(std::memory_order_relaxed);
    out.submissions_global = submissions_global_.load(std::memory_order_relaxed);
    return out;
}

} // namespace thunderbolt
