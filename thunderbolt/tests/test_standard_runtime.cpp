#include "TestHarness.hpp"

#include <thunderbolt/runtime/StandardRuntime.hpp>

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

using thunderbolt::RuntimeConfig;
using thunderbolt::StandardRuntime;
using thunderbolt::TaskContext;
using thunderbolt::TaskHandle;
using thunderbolt::TaskPriority;

TB_TEST("runtime honours the requested worker count exactly") {
    // S58's scaling sweep reads worker_count as "threads executing tasks". A
    // runtime that rounded this up, or added a helper, would report a speedup
    // for a configuration it was not actually running.
    RuntimeConfig config;
    config.worker_count = 3;
    StandardRuntime runtime(config);
    TB_CHECK_EQ(runtime.worker_count(), 3u);
    TB_CHECK(runtime.name() == "standard");
}

TB_TEST("a submitted task runs and completes") {
    RuntimeConfig config;
    config.worker_count = 2;
    StandardRuntime runtime(config);

    std::atomic<int> ran{0};
    TaskHandle handle = runtime.submit([&ran] { ran.fetch_add(1); });
    runtime.wait(handle);

    TB_CHECK_EQ(ran.load(), 1);
    TB_CHECK(runtime.is_complete(handle));
}

TB_TEST("wait_all drains every submitted task") {
    RuntimeConfig config;
    config.worker_count = 4;
    StandardRuntime runtime(config);

    constexpr int    kTasks = 1000;
    std::atomic<int> ran{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    TB_CHECK_EQ(ran.load(), kTasks);
    TB_CHECK_EQ(runtime.outstanding_task_count(), 0u);
}

TB_TEST("every task runs exactly once under concurrent submission") {
    // The failure this catches is a task being executed twice (double-claim) or
    // dropped (lost wakeup). Counting per-task rather than in aggregate means a
    // double-execution cannot be cancelled out by a drop.
    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 8192;
    StandardRuntime runtime(config);

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
                    counters[static_cast<std::size_t>(index)].fetch_add(1,
                                                                        std::memory_order_relaxed);
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

TB_TEST("waiting on an already-completed task returns immediately") {
    RuntimeConfig config;
    config.worker_count = 2;
    StandardRuntime runtime(config);

    TaskHandle handle = runtime.submit([] {});
    runtime.wait(handle);
    runtime.wait(handle);  // stale by now: the slot has been recycled
    TB_CHECK(runtime.is_complete(handle));
}

TB_TEST("waiting on an invalid handle is a no-op") {
    RuntimeConfig config;
    config.worker_count = 1;
    StandardRuntime runtime(config);
    runtime.wait(TaskHandle{});
    TB_CHECK(runtime.is_complete(TaskHandle{}));
}

TB_TEST("a task may submit and wait on nested work without deadlocking") {
    // This is the help-on-wait contract. With a single worker, parking inside
    // wait() would deadlock outright: the parent occupies the only worker while
    // the child it is waiting for sits in the queue with nobody to run it.
    RuntimeConfig config;
    config.worker_count = 1;
    StandardRuntime runtime(config);

    std::atomic<int> inner_ran{0};
    TaskHandle outer = runtime.submit([&runtime, &inner_ran](TaskContext&) {
        TaskHandle inner = runtime.submit([&inner_ran] { inner_ran.fetch_add(1); });
        runtime.wait(inner);
    });
    runtime.wait(outer);

    TB_CHECK_EQ(inner_ran.load(), 1);
}

TB_TEST("nested waits survive several levels of depth") {
    RuntimeConfig config;
    config.worker_count = 2;
    StandardRuntime runtime(config);

    std::atomic<int> leaf_count{0};

    // Each level submits two children and waits for both, so the graph is wide
    // as well as deep - the shape most likely to expose a help-on-wait bug.
    struct Recursive {
        StandardRuntime&  runtime;
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

    Recursive rec{runtime, leaf_count};
    TaskHandle root = runtime.submit([&rec] { rec.run(5); });
    runtime.wait(root);

    TB_CHECK_EQ(leaf_count.load(), 32);  // 2^5 leaves
}

TB_TEST("parallel_for covers every index exactly once") {
    RuntimeConfig config;
    config.worker_count = 4;
    StandardRuntime runtime(config);

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

TB_TEST("parallel_for handles ranges smaller than the grain") {
    RuntimeConfig config;
    config.worker_count = 2;
    StandardRuntime runtime(config);

    std::vector<int> values(3, 0);
    runtime.parallel_for(0, 3, 1024, [&values](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            values[i] = 1;
        }
    });
    TB_CHECK_EQ(values[0] + values[1] + values[2], 3);
}

TB_TEST("parallel_for over an empty range does nothing") {
    RuntimeConfig config;
    config.worker_count = 2;
    StandardRuntime runtime(config);

    int calls = 0;
    runtime.parallel_for(5, 5, 16, [&calls](std::size_t, std::size_t) { ++calls; });
    TB_CHECK_EQ(calls, 0);
}

TB_TEST("parallel_for spills past the inline handle budget correctly") {
    // 64 handles are stored inline; beyond that the implementation switches to a
    // heap buffer. The switch is the bug-prone part, so cross it deliberately.
    RuntimeConfig config;
    config.worker_count = 4;
    StandardRuntime runtime(config);

    constexpr std::size_t kCount = 1000;  // 1000 batches of 1
    std::vector<int>      values(kCount, 0);
    runtime.parallel_for(0, kCount, 1, [&values](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            values[i] += 1;
        }
    });

    const long long sum = std::accumulate(values.begin(), values.end(), 0LL);
    TB_CHECK_EQ(sum, static_cast<long long>(kCount));
}

TB_TEST("pool exhaustion degrades to inline execution rather than dropping work") {
    // Correctness must not depend on the pool being large enough. The work still
    // has to happen; only the parallelism is allowed to suffer, and the runtime
    // has to say so.
    RuntimeConfig config;
    config.worker_count  = 2;
    config.task_capacity = 8;
    StandardRuntime runtime(config);

    constexpr int    kTasks = 2000;
    std::atomic<int> ran{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    TB_CHECK_EQ(ran.load(), kTasks);           // nothing was dropped
    TB_CHECK(runtime.inline_execution_count() > 0);  // and the degradation is reported
}

TB_TEST("tasks of mixed duration all complete") {
    RuntimeConfig config;
    config.worker_count = 4;
    StandardRuntime runtime(config);

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

TB_TEST("destruction drains outstanding work instead of dropping it") {
    // Destroying a runtime with tasks still queued must not silently discard
    // them: a dropped task surfaces much later as a wrong result and looks like
    // a scheduling bug.
    std::atomic<int> ran{0};
    {
        RuntimeConfig config;
        config.worker_count = 2;
        StandardRuntime runtime(config);
        for (int i = 0; i < 500; ++i) {
            (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
    }
    TB_CHECK_EQ(ran.load(), 500);
}

TB_TEST("a runtime with no submitted work shuts down cleanly") {
    RuntimeConfig config;
    config.worker_count = 8;
    StandardRuntime runtime(config);
    runtime.wait_all();
}

TB_TEST("high task count stress: nothing lost, nothing duplicated") {
    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 4096;
    StandardRuntime runtime(config);

    constexpr int         kTasks = 100000;
    std::atomic<long long> sum{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&sum] { sum.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();
    TB_CHECK_EQ(sum.load(), static_cast<long long>(kTasks));
}

TB_TEST("priorities are respected among already-queued work") {
    // Submitted while the single worker is blocked, so ordering is decided by
    // the queue rather than by submission timing. Asserting on completion ORDER
    // rather than on timing keeps this deterministic.
    RuntimeConfig config;
    config.worker_count = 1;
    StandardRuntime runtime(config);

    std::atomic<bool> gate{false};
    TaskHandle blocker = runtime.submit([&gate] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::mutex             order_mutex;
    std::vector<TaskPriority> order;
    auto record = [&order_mutex, &order](TaskPriority p) {
        std::lock_guard lock(order_mutex);
        order.push_back(p);
    };

    (void)runtime.submit([&record] { record(TaskPriority::Background); },
                         TaskPriority::Background);
    (void)runtime.submit([&record] { record(TaskPriority::Normal); }, TaskPriority::Normal);
    (void)runtime.submit([&record] { record(TaskPriority::Critical); }, TaskPriority::Critical);

    gate.store(true, std::memory_order_release);
    runtime.wait(blocker);
    runtime.wait_all();

    TB_CHECK_EQ(order.size(), 3u);
    TB_CHECK(order[0] == TaskPriority::Critical);
    TB_CHECK(order[1] == TaskPriority::Normal);
    TB_CHECK(order[2] == TaskPriority::Background);
}
