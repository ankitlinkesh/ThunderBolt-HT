// Thunderbolt HT - the work-stealing runtime.
//
// S9: per-worker deques are the primary execution mechanism; the global queue
// exists only for externally submitted work and for local overflow. A worker
// pushes and pops its own deque at the bottom with no atomics contended in the
// common case, and reaches for another worker's top only when it runs dry.
//
// Everything that is not work DISTRIBUTION comes from RuntimeBase, so this class
// and StandardRuntime share one implementation of task lifetime, completion,
// waiting and help-on-wait. The A/B comparison between them therefore isolates
// scheduling, which is the only way its results mean anything.
#pragma once

#include <thunderbolt/core/executor/AbpDeque.hpp>
#include <thunderbolt/core/executor/GlobalQueue.hpp>
#include <thunderbolt/runtime/RuntimeBase.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace thunderbolt {

// Counters describing what the scheduler actually did. S53 requires these to be
// real measurements; they are also the evidence for whether stealing is helping
// or just burning cycles.
struct ThunderboltStats {
    std::uint64_t tasks_executed        = 0;
    std::uint64_t steal_attempts        = 0;
    std::uint64_t steals_succeeded      = 0;
    std::uint64_t steals_failed         = 0;  // victim empty or lost the race
    std::uint64_t global_pops           = 0;
    std::uint64_t local_overflows       = 0;  // deque full; task went to the global queue
    std::uint64_t worker_parks          = 0;
    std::uint64_t submissions_local     = 0;
    std::uint64_t submissions_global    = 0;
};

class ThunderboltRuntime final : public RuntimeBase {
public:
    explicit ThunderboltRuntime(RuntimeConfig config = {});
    ~ThunderboltRuntime() override;

    using ITaskRuntime::submit;

    [[nodiscard]] TaskHandle submit(TaskDesc desc) override;

    [[nodiscard]] std::uint32_t    worker_count() const noexcept override { return worker_count_; }
    [[nodiscard]] std::string_view name() const noexcept override { return "thunderbolt"; }

    [[nodiscard]] ThunderboltStats stats() const;

    // Whether each worker's affinity request was actually honoured. A pinning
    // request the OS refused must not be reported as a pinned run.
    [[nodiscard]] std::uint32_t pinned_worker_count() const noexcept {
        return pinned_workers_.load(std::memory_order_relaxed);
    }

protected:
    bool               try_execute_one(TaskContext& ctx) override;
    [[nodiscard]] bool on_own_worker(std::uint32_t& out_index) const override;

private:
    // Per-worker state, cache-line aligned so that one worker's counters never
    // share a line with another's - false sharing here would show up as
    // scheduler overhead and be measured as such.
    TB_BEGIN_CACHE_ALIGNED_TYPE
    struct alignas(kCacheLineSize) WorkerState {
        // One deque per priority. Priority has to be honoured here, not deferred,
        // because StandardRuntime honours it: a semantic difference between the
        // two runtimes would be measured as a scheduling difference.
        std::unique_ptr<AbpDeque<TaskHandle>> queues[kPriorityCount];

        // xorshift64 state for victim selection. Owner-only, so no atomics.
        std::uint64_t rng = 0;

        std::atomic<std::uint64_t> executed{0};
        std::atomic<std::uint64_t> steal_attempts{0};
        std::atomic<std::uint64_t> steals_succeeded{0};
        std::atomic<std::uint64_t> steals_failed{0};
        std::atomic<std::uint64_t> global_pops{0};
        std::atomic<std::uint64_t> parks{0};
    };
    TB_END_CACHE_ALIGNED_TYPE

    void worker_loop(std::uint32_t worker_index);

    // Finds the next task for `worker`, in the order local -> global -> steal.
    // Returns an invalid handle when nothing is available anywhere.
    [[nodiscard]] TaskHandle acquire_next(WorkerState& worker, std::uint32_t worker_index);

    [[nodiscard]] TaskHandle pop_local(WorkerState& worker);
    [[nodiscard]] TaskHandle steal_from_others(WorkerState& worker, std::uint32_t worker_index);

    // Moves a batch of tasks from the global queue into the worker's own deque
    // and returns one to run. Taking a batch keeps the global lock off the hot
    // path: eight workers popping single tasks would turn the injection queue
    // into exactly the contended bottleneck S9 warns against.
    [[nodiscard]] TaskHandle drain_global(WorkerState& worker);

    [[nodiscard]] bool any_work_available() const;

    void wake_one_worker();
    void park_worker(WorkerState& worker);

    std::uint32_t worker_count_ = 0;

    std::vector<std::unique_ptr<WorkerState>> workers_;
    std::vector<std::thread>                  threads_;

    GlobalQueue global_;

    std::atomic<std::uint64_t> local_overflows_{0};
    std::atomic<std::uint64_t> submissions_local_{0};
    std::atomic<std::uint64_t> submissions_global_{0};
    std::atomic<std::uint32_t> pinned_workers_{0};

    // Parking. Workers spin briefly before sleeping, because on a frame-based
    // workload the next task usually arrives within microseconds and a
    // condition-variable round trip costs more than the spin.
    mutable std::mutex         sleep_mutex_;
    std::condition_variable    sleep_cv_;
    std::atomic<std::uint32_t> sleeping_{0};

    std::atomic<bool> running_{false};
};

} // namespace thunderbolt
