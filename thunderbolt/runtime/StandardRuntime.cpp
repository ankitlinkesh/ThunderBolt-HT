#include <thunderbolt/runtime/StandardRuntime.hpp>

#include <thunderbolt/cpu/affinity/Affinity.hpp>

#include <cassert>
#include <utility>

namespace thunderbolt {
namespace {

// Identifies the runtime a thread works for, and which worker it is. Keyed on the
// runtime instance so that nesting two runtimes stays unambiguous.
struct WorkerIdentity {
    const void*   owner = nullptr;
    std::uint32_t index = TaskContext::kExternalThread;
};

thread_local WorkerIdentity t_identity;

} // namespace

StandardRuntime::StandardRuntime(RuntimeConfig config) : RuntimeBase(config) {
    worker_count_ = resolve_worker_count(config);

    running_.store(true, std::memory_order_release);

    workers_.reserve(worker_count_);
    for (std::uint32_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
    }
}

StandardRuntime::~StandardRuntime() {
    // Drain before stopping. Destroying a runtime with work still queued would
    // silently drop tasks, and a dropped task is indistinguishable from a
    // scheduling bug when it surfaces later as a wrong result.
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

    TaskHandle handle = acquire_task(std::move(desc));

    if (!handle.valid()) {
        // Pool exhausted. Degrade to inline execution rather than failing or
        // reaching for the allocator: the work still happens and stays correct,
        // only the parallelism suffers, and the counter makes that visible.
        run_inline(std::move(desc));
        return TaskHandle{};
    }

    Task* task = pool().get(handle);
    assert(task != nullptr);
    task->state.store(TaskState::Queued, std::memory_order_release);

    {
        std::lock_guard lock(queue_mutex_);
        ready_[static_cast<std::size_t>(priority)].push_back(handle);
        ++queued_count_;
    }
    // notify_one, not notify_all: waking every worker for a single task creates a
    // thundering herd that contends on the queue lock and goes straight back to
    // sleep.
    queue_cv_.notify_one();

    return handle;
}

TaskHandle StandardRuntime::pop_locked() {
    if (queued_count_ == 0) {
        return TaskHandle{};
    }
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

    // The baseline honours the affinity knob too. A setting that applied to only
    // one runtime could not be held constant across an A/B comparison, which
    // would make it a confound rather than a variable.
    if (apply_worker_affinity(worker_index, config().affinity)) {
        pinned_workers_.fetch_add(1, std::memory_order_relaxed);
    }

    TaskContext ctx{this, worker_index};

    while (running_.load(std::memory_order_acquire)) {
        TaskHandle handle;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return queued_count_ > 0 || !running_.load(std::memory_order_acquire);
            });

            if (queued_count_ == 0) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;  // woken for shutdown
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

} // namespace thunderbolt
