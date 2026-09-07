// Thunderbolt HT - behaviour every ITaskRuntime must exhibit identically.
//
// WHY THESE ARE TEMPLATES. The A/B comparison is only valid if StandardRuntime
// and ThunderboltRuntime agree on what the API MEANS - when a task is complete,
// what wait() does from inside a worker, whether priorities are honoured, what
// happens when the pool is exhausted. If they disagree, the benchmark measures
// the disagreement and reports it as a scheduling result.
//
// Testing each runtime with its own hand-written suite would let them drift apart
// silently, because nobody notices a test that only ever existed for one of them.
// Running ONE suite against both makes divergence a build failure.
#pragma once

#include "TestHarness.hpp"

#include <thunderbolt/api/ITaskRuntime.hpp>
#include <thunderbolt/api/RuntimeConfig.hpp>
#include <thunderbolt/api/TaskContext.hpp>
#include <thunderbolt/api/TaskHandle.hpp>
#include <thunderbolt/api/TaskTypes.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace thunderbolt::conformance {

template <typename R>
void honours_worker_count() {
    // S58's scaling sweep reads worker_count as "threads executing tasks". A
    // runtime that rounded this up, or added a helper, would report a speedup for
    // a configuration it never actually ran.
    RuntimeConfig config;
    config.worker_count = 3;
    R runtime(config);
    TB_CHECK_EQ(runtime.worker_count(), 3u);
}

template <typename R>
void submitted_task_runs_and_completes() {
    RuntimeConfig config;
    config.worker_count = 2;
    R runtime(config);

    std::atomic<int> ran{0};
    TaskHandle       handle = runtime.submit([&ran] { ran.fetch_add(1); });
    runtime.wait(handle);

    TB_CHECK_EQ(ran.load(), 1);
    TB_CHECK(runtime.is_complete(handle));
}

template <typename R>
void wait_all_drains_everything() {
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    constexpr int    kTasks = 1000;
    std::atomic<int> ran{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    TB_CHECK_EQ(ran.load(), kTasks);
    TB_CHECK_EQ(runtime.outstanding_task_count(), 0u);
}

template <typename R>
void every_task_runs_exactly_once() {
    // Catches a task executed twice (double-claim) or dropped (lost wakeup).
    // Counting PER TASK rather than in aggregate means a double-execution cannot
    // be cancelled out by a drop and go unnoticed.
    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 8192;
    R runtime(config);

    constexpr int kSubmitters = 4;
    constexpr int kPerThread  = 500;
    constexpr int kTotal      = kSubmitters * kPerThread;

    std::vector<std::atomic<int>> counters(kTotal);
    for (auto& c : counters) {
        c.store(0);
    }

    std::vector<std::thread> submitters;
    for (int t = 0; t < kSubmitters; ++t) {
        submitters.emplace_back([&runtime, &counters, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const int index = t * kPerThread + i;
                (void)runtime.submit([&counters, index] {
                    counters[static_cast<std::size_t>(index)].fetch_add(
                        1, std::memory_order_relaxed);
                });
            }
        });
    }
    for (std::thread& th : submitters) {
        th.join();
    }
    runtime.wait_all();

    for (int i = 0; i < kTotal; ++i) {
        TB_CHECK_EQ(counters[static_cast<std::size_t>(i)].load(), 1);
    }
}

template <typename R>
void waiting_on_completed_task_returns() {
    RuntimeConfig config;
    config.worker_count = 2;
    R runtime(config);

    TaskHandle handle = runtime.submit([] {});
    runtime.wait(handle);
    runtime.wait(handle);  // stale by now: the slot has been recycled
    TB_CHECK(runtime.is_complete(handle));
}

template <typename R>
void waiting_on_invalid_handle_is_noop() {
    RuntimeConfig config;
    config.worker_count = 1;
    R runtime(config);
    runtime.wait(TaskHandle{});
    TB_CHECK(runtime.is_complete(TaskHandle{}));
}

template <typename R>
void nested_submit_and_wait_does_not_deadlock() {
    // The help-on-wait contract. With a single worker, parking inside wait()
    // deadlocks outright: the parent occupies the only worker while the child it
    // waits for sits in a queue with nobody left to run it.
    RuntimeConfig config;
    config.worker_count = 1;
    R runtime(config);

    std::atomic<int> inner_ran{0};
    TaskHandle       outer = runtime.submit([&runtime, &inner_ran](TaskContext&) {
        TaskHandle inner = runtime.submit([&inner_ran] { inner_ran.fetch_add(1); });
        runtime.wait(inner);
    });
    runtime.wait(outer);

    TB_CHECK_EQ(inner_ran.load(), 1);
}

template <typename R>
void nested_waits_survive_depth() {
    RuntimeConfig config;
    config.worker_count = 2;
    R runtime(config);

    std::atomic<int> leaf_count{0};

    // Wide as well as deep - the shape most likely to expose a help-on-wait bug.
    //
    // NOTE: `rec` lives on this stack frame and tasks capture it by pointer. Safe
    // ONLY because every level waits for its children before returning. If this
    // is ever adapted to fire-and-forget submission - which Phase D's dependency
    // edges make natural - the captured reference outlives the frame.
    struct Recursive {
        R&                runtime;
        std::atomic<int>& leaves;

        void run(int depth) {
            if (depth == 0) {
                leaves.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            TaskHandle a = runtime.submit([this, depth] { run(depth - 1); });
            TaskHandle b = runtime.submit([this, depth] { run(depth - 1); });
            runtime.wait(a);
            runtime.wait(b);
        }
    };

    Recursive  rec{runtime, leaf_count};
    TaskHandle root = runtime.submit([&rec] { rec.run(5); });
    runtime.wait(root);

    TB_CHECK_EQ(leaf_count.load(), 32);  // 2^5 leaves
}

template <typename R>
void concurrent_handle_waiters_on_different_tasks_all_wake() {
    // RuntimeBase targets wakeups at the specific task a handle-waiter is
    // blocked on, rather than waking every waiter on every completion (the
    // wakeup-storm shape already fixed once for wait_all() - see the comment
    // above handle_waiter_slots_ in RuntimeBase.hpp). That targeting is only
    // correct if EVERY waiter's own completion still reaches it, so this drives
    // more concurrent external-thread waiters (48) than the fixed slot budget
    // (32) holds, forcing both the per-slot path and the overflow fallback to
    // run in the same test. A lost wakeup here is a permanent hang, not a wrong
    // answer, so this test cannot fail quietly - a hung thread trips the test
    // binary's own timeout.
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    constexpr int kWaiters = 48;  // > kMaxHandleWaiterSlots (32)

    std::vector<std::atomic<bool>> gate(kWaiters);
    for (auto& g : gate) {
        g.store(false);
    }

    std::vector<TaskHandle> handles(kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
        handles[static_cast<std::size_t>(i)] = runtime.submit([&gate, i] {
            // Each task blocks briefly so every waiter is genuinely parked in
            // wait() - not racing to find its task already complete - by the
            // time the last task is submitted below.
            while (!gate[static_cast<std::size_t>(i)].load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }

    std::atomic<int> woken{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&runtime, &handles, &woken, i] {
            runtime.wait(handles[static_cast<std::size_t>(i)]);
            woken.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Release every task only once all 48 waiter threads are plausibly parked.
    // Not a guarantee - the point of the test is that late arrivals and
    // overflow are ALSO handled correctly, not just the well-timed case.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (auto& g : gate) {
        g.store(true, std::memory_order_release);
    }

    for (std::thread& th : waiters) {
        th.join();
    }

    TB_CHECK_EQ(woken.load(), kWaiters);
}

template <typename R>
void parallel_for_covers_every_index_once() {
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    constexpr std::size_t kCount = 10000;
    std::vector<int>      values(kCount, 0);

    runtime.parallel_for(0, kCount, 256, [&values](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            values[i] += 1;
        }
    });

    const long long sum = std::accumulate(values.begin(), values.end(), 0LL);
    TB_CHECK_EQ(sum, static_cast<long long>(kCount));
}

template <typename R>
void parallel_for_handles_small_and_empty_ranges() {
    RuntimeConfig config;
    config.worker_count = 2;
    R runtime(config);

    std::vector<int> values(3, 0);
    runtime.parallel_for(0, 3, 1024, [&values](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            values[i] = 1;
        }
    });
    TB_CHECK_EQ(values[0] + values[1] + values[2], 3);

    int calls = 0;
    runtime.parallel_for(5, 5, 16, [&calls](std::size_t, std::size_t) { ++calls; });
    TB_CHECK_EQ(calls, 0);
}

template <typename R>
void parallel_for_partitions_by_worker_count_not_batch_count() {
    // Phase I Stage 6: parallel_for no longer submits one task per batch. It
    // submits at most min(batch_count, worker_count()) tasks, each claiming
    // grain-sized chunks from a shared cursor. With grain=1 over 1000 items and
    // 4 workers that is ~1000 batches but only 4 tasks - the whole point of the
    // partitioning path, and the thing a regression back to per-batch submission
    // would break silently (correctness would still pass; only cost would regress).
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    constexpr std::size_t kCount = 1000;  // 1000 possible batches of 1

    const std::uint64_t acquires_before = runtime.pool_acquire_count();

    std::vector<int> values(kCount, 0);
    runtime.parallel_for(0, kCount, 1, [&values](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            values[i] += 1;
        }
    });

    const long long sum = std::accumulate(values.begin(), values.end(), 0LL);
    TB_CHECK_EQ(sum, static_cast<long long>(kCount));

    // 4 partition tasks, not 1000 batch tasks. A generous ceiling (worker_count
    // plus a small margin for any bookkeeping task the runtime itself submits)
    // rather than an exact count, so this does not pin an implementation detail
    // unrelated to what Stage 6 actually promises.
    const std::uint64_t acquires_after = runtime.pool_acquire_count();
    TB_CHECK(acquires_after - acquires_before <= 16u);
}

template <typename R>
void parallel_for_spills_past_inline_handles() {
    // 64 handles are stored inline; beyond that the implementation switches to a
    // heap buffer. Since Stage 6, task_count is bounded by worker_count() rather
    // than by batch_count, so crossing that boundary now requires MORE WORKERS
    // than the inline budget rather than more batches. Exotic, but correctness
    // must not depend on how exotic.
    RuntimeConfig config;
    config.worker_count = 96;
    R runtime(config);

    constexpr std::size_t kCount = 5000;  // several chunks per worker
    std::vector<int>      values(kCount, 0);
    runtime.parallel_for(0, kCount, 1, [&values](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            values[i] += 1;
        }
    });

    const long long sum = std::accumulate(values.begin(), values.end(), 0LL);
    TB_CHECK_EQ(sum, static_cast<long long>(kCount));
}

template <typename R>
void pool_exhaustion_degrades_but_never_drops() {
    // Correctness must not depend on the pool being large enough. The work still
    // has to happen; only parallelism may suffer, and the runtime must say so.
    RuntimeConfig config;
    config.worker_count  = 2;
    config.task_capacity = 8;
    R runtime(config);

    constexpr int    kTasks = 2000;
    std::atomic<int> ran{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    TB_CHECK_EQ(ran.load(), kTasks);                 // nothing was dropped
    TB_CHECK(runtime.inline_execution_count() > 0);  // and the degradation is reported
}

template <typename R>
void mixed_duration_tasks_all_complete() {
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    std::atomic<int> ran{0};
    for (int i = 0; i < 200; ++i) {
        (void)runtime.submit([&ran, i] {
            if (i % 50 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ran.fetch_add(1, std::memory_order_relaxed);
        });
    }
    runtime.wait_all();
    TB_CHECK_EQ(ran.load(), 200);
}

template <typename R>
void destruction_drains_rather_than_drops() {
    // Destroying a runtime with tasks still queued must not discard them: a
    // dropped task surfaces much later as a wrong result and looks like a
    // scheduling bug.
    std::atomic<int> ran{0};
    {
        RuntimeConfig config;
        config.worker_count = 2;
        R runtime(config);
        for (int i = 0; i < 500; ++i) {
            (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
    }
    TB_CHECK_EQ(ran.load(), 500);
}

template <typename R>
void idle_runtime_shuts_down_cleanly() {
    RuntimeConfig config;
    config.worker_count = 8;
    R runtime(config);
    runtime.wait_all();
}

template <typename R>
void high_task_count_stress() {
    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 4096;
    R runtime(config);

    constexpr int          kTasks = 100000;
    std::atomic<long long> sum{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&sum] { sum.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();
    TB_CHECK_EQ(sum.load(), static_cast<long long>(kTasks));
}

template <typename R>
void priorities_are_respected() {
    // One worker, so ordering is decided by the priority scan rather than by which
    // core happened to be free. Both runtimes must agree here: if only one
    // honoured priority, the difference would be measured as a scheduling result
    // when it is really a semantic difference.
    RuntimeConfig config;
    config.worker_count = 1;
    R runtime(config);

    std::atomic<bool> gate{false};
    std::atomic<bool> blocker_running{false};
    TaskHandle        blocker = runtime.submit([&gate, &blocker_running] {
        blocker_running.store(true, std::memory_order_release);
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    // Block until the only worker is demonstrably INSIDE the blocker. Without
    // this it could start draining prioritized tasks as they arrive, and the
    // recorded order would reflect submission timing rather than priority - a
    // test that passes for the wrong reason and keeps passing after a scheduler
    // change breaks priorities.
    while (!blocker_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::mutex                order_mutex;
    std::vector<TaskPriority> order;
    auto record = [&order_mutex, &order](TaskPriority p) {
        std::lock_guard lock(order_mutex);
        order.push_back(p);
    };

    (void)runtime.submit([&record] { record(TaskPriority::Background); },
                         TaskPriority::Background);
    (void)runtime.submit([&record] { record(TaskPriority::Normal); }, TaskPriority::Normal);
    (void)runtime.submit([&record] { record(TaskPriority::Critical); }, TaskPriority::Critical);

    // Blocker plus three queued tasks. Asserting this BEFORE opening the gate is
    // what makes the expected order follow from the priority scan alone.
    TB_CHECK_EQ(runtime.outstanding_task_count(), 4u);

    gate.store(true, std::memory_order_release);
    runtime.wait(blocker);
    runtime.wait_all();

    TB_CHECK_EQ(order.size(), 3u);
    TB_CHECK(order[0] == TaskPriority::Critical);
    TB_CHECK(order[1] == TaskPriority::Normal);
    TB_CHECK(order[2] == TaskPriority::Background);
}

} // namespace thunderbolt::conformance

// Registers the whole suite for one runtime type. Adding a runtime means adding
// one line; forgetting to test it is not an option the structure allows.
#define TB_RUNTIME_CONFORMANCE_SUITE(RuntimeT, Label)                                            \
    TB_TEST(Label " honours the requested worker count") {                                       \
        ::thunderbolt::conformance::honours_worker_count<RuntimeT>();                            \
    }                                                                                            \
    TB_TEST(Label " runs and completes a submitted task") {                                      \
        ::thunderbolt::conformance::submitted_task_runs_and_completes<RuntimeT>();               \
    }                                                                                            \
    TB_TEST(Label " wait_all drains every submitted task") {                                     \
        ::thunderbolt::conformance::wait_all_drains_everything<RuntimeT>();                      \
    }                                                                                            \
    TB_TEST(Label " runs every task exactly once under concurrent submission") {                 \
        ::thunderbolt::conformance::every_task_runs_exactly_once<RuntimeT>();                    \
    }                                                                                            \
    TB_TEST(Label " waiting on an already-completed task returns immediately") {                 \
        ::thunderbolt::conformance::waiting_on_completed_task_returns<RuntimeT>();               \
    }                                                                                            \
    TB_TEST(Label " waiting on an invalid handle is a no-op") {                                  \
        ::thunderbolt::conformance::waiting_on_invalid_handle_is_noop<RuntimeT>();               \
    }                                                                                            \
    TB_TEST(Label " nested submit and wait does not deadlock") {                                 \
        ::thunderbolt::conformance::nested_submit_and_wait_does_not_deadlock<RuntimeT>();        \
    }                                                                                            \
    TB_TEST(Label " nested waits survive several levels of depth") {                             \
        ::thunderbolt::conformance::nested_waits_survive_depth<RuntimeT>();                      \
    }                                                                                            \
    TB_TEST(Label " concurrent handle waiters on different tasks all wake") {                    \
        ::thunderbolt::conformance::concurrent_handle_waiters_on_different_tasks_all_wake<        \
            RuntimeT>();                                                                          \
    }                                                                                            \
    TB_TEST(Label " parallel_for covers every index exactly once") {                             \
        ::thunderbolt::conformance::parallel_for_covers_every_index_once<RuntimeT>();            \
    }                                                                                            \
    TB_TEST(Label " parallel_for handles small and empty ranges") {                              \
        ::thunderbolt::conformance::parallel_for_handles_small_and_empty_ranges<RuntimeT>();     \
    }                                                                                            \
    TB_TEST(Label " parallel_for spills past the inline handle budget") {                        \
        ::thunderbolt::conformance::parallel_for_spills_past_inline_handles<RuntimeT>();         \
    }                                                                                            \
    TB_TEST(Label " parallel_for partitions by worker count, not batch count") {                 \
        ::thunderbolt::conformance::parallel_for_partitions_by_worker_count_not_batch_count<      \
            RuntimeT>();                                                                          \
    }                                                                                            \
    TB_TEST(Label " pool exhaustion degrades without dropping work") {                           \
        ::thunderbolt::conformance::pool_exhaustion_degrades_but_never_drops<RuntimeT>();        \
    }                                                                                            \
    TB_TEST(Label " tasks of mixed duration all complete") {                                     \
        ::thunderbolt::conformance::mixed_duration_tasks_all_complete<RuntimeT>();               \
    }                                                                                            \
    TB_TEST(Label " destruction drains outstanding work") {                                      \
        ::thunderbolt::conformance::destruction_drains_rather_than_drops<RuntimeT>();            \
    }                                                                                            \
    TB_TEST(Label " an idle runtime shuts down cleanly") {                                       \
        ::thunderbolt::conformance::idle_runtime_shuts_down_cleanly<RuntimeT>();                 \
    }                                                                                            \
    TB_TEST(Label " high task count stress loses nothing") {                                     \
        ::thunderbolt::conformance::high_task_count_stress<RuntimeT>();                          \
    }                                                                                            \
    TB_TEST(Label " priorities are respected among queued work") {                               \
        ::thunderbolt::conformance::priorities_are_respected<RuntimeT>();                        \
    }
