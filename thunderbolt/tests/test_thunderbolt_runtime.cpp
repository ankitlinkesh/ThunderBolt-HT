// Behaviour specific to the work-stealing runtime.
//
// Shared semantics are covered once, for both runtimes, in
// test_runtime_conformance.cpp. What is left here is what only ThunderboltRuntime
// claims to do - and the point of these tests is that the claims are checked
// rather than assumed. "Work stealing is enabled" is not the same statement as
// "work was actually stolen".
#include "TestHarness.hpp"

#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using thunderbolt::AffinityMode;
using thunderbolt::RuntimeConfig;
using thunderbolt::TaskContext;
using thunderbolt::TaskHandle;
using thunderbolt::ThunderboltRuntime;

TB_TEST("work is actually stolen, not merely enabled") {
    // Every task is submitted from ONE worker, so all of them land on that
    // worker's local deque. If stealing did not work, the other seven workers
    // would sit idle and this would still pass a "did all tasks run?" check -
    // which is exactly why that check is not sufficient. Asserting a non-zero
    // steal count is what makes the mechanism observable.
    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 16384;
    ThunderboltRuntime runtime(config);

    constexpr int    kChildren = 2000;
    std::atomic<int> ran{0};

    TaskHandle parent = runtime.submit([&runtime, &ran](TaskContext&) {
        std::vector<TaskHandle> children;
        children.reserve(kChildren);
        for (int i = 0; i < kChildren; ++i) {
            children.push_back(runtime.submit([&ran] {
                // A little work, so a thief has something worth taking.
                volatile int sink = 0;
                for (int k = 0; k < 200; ++k) {
                    sink += k;
                }
                ran.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        for (TaskHandle h : children) {
            runtime.wait(h);
        }
    });
    runtime.wait(parent);

    TB_CHECK_EQ(ran.load(), kChildren);

    const auto stats = runtime.stats();
    TB_CHECK(stats.steals_succeeded > 0);
    TB_CHECK(stats.tasks_executed >= static_cast<std::uint64_t>(kChildren));
}

TB_TEST("externally submitted work reaches the global queue") {
    // Submissions from outside a worker have no local deque to go to, so they
    // must be routed globally. If they were silently dropped or misrouted the
    // conformance suite would still pass, because the tasks would run either way.
    RuntimeConfig config;
    config.worker_count = 2;
    ThunderboltRuntime runtime(config);

    std::atomic<int> ran{0};
    for (int i = 0; i < 100; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    TB_CHECK_EQ(ran.load(), 100);
    const auto stats = runtime.stats();
    TB_CHECK_EQ(stats.submissions_global, 100u);
    TB_CHECK_EQ(stats.submissions_local, 0u);
}

TB_TEST("work submitted from inside a task stays local") {
    // The locality claim behind work stealing: children are queued on the worker
    // that produced them and only migrate under load.
    RuntimeConfig config;
    config.worker_count = 4;
    ThunderboltRuntime runtime(config);

    std::atomic<int> ran{0};
    TaskHandle parent = runtime.submit([&runtime, &ran](TaskContext&) {
        std::vector<TaskHandle> children;
        for (int i = 0; i < 50; ++i) {
            children.push_back(
                runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
        }
        for (TaskHandle h : children) {
            runtime.wait(h);
        }
    });
    runtime.wait(parent);

    TB_CHECK_EQ(ran.load(), 50);
    const auto stats = runtime.stats();
    TB_CHECK(stats.submissions_local > 0);
}

TB_TEST("local deque overflow spills to the global queue instead of failing") {
    // The deque is bounded, so overflow is a designed-for state rather than an
    // error. The work must still run, and the runtime must report that its
    // capacity was exceeded rather than hiding it.
    RuntimeConfig config;
    config.worker_count  = 1;
    config.task_capacity = 65536;
    ThunderboltRuntime runtime(config);

    // Per-priority deque capacity is 1024; submit well past it from one worker
    // without waiting, so nothing drains in between.
    constexpr int    kChildren = 5000;
    std::atomic<int> ran{0};

    TaskHandle parent = runtime.submit([&runtime, &ran](TaskContext&) {
        std::vector<TaskHandle> children;
        children.reserve(kChildren);
        for (int i = 0; i < kChildren; ++i) {
            children.push_back(
                runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
        }
        for (TaskHandle h : children) {
            runtime.wait(h);
        }
    });
    runtime.wait(parent);

    TB_CHECK_EQ(ran.load(), kChildren);
    const auto stats = runtime.stats();
    TB_CHECK(stats.local_overflows > 0);       // capacity really was exceeded
    TB_CHECK(stats.submissions_global > 0);    // and the overflow went somewhere
}

TB_TEST("idle workers park and are woken by new work") {
    // Parking is what keeps an idle runtime off the CPU - important on a 15 W
    // part where a spinning worker heats the package and throttles the ones doing
    // real work. The risk is a lost wakeup, so this checks that a runtime which
    // has definitely gone to sleep still picks up later submissions.
    RuntimeConfig config;
    config.worker_count = 4;
    ThunderboltRuntime runtime(config);

    // Long enough for every worker to exhaust its spin budget and park.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TB_CHECK(runtime.stats().worker_parks > 0);

    std::atomic<int> ran{0};
    for (int i = 0; i < 200; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();
    TB_CHECK_EQ(ran.load(), 200);

    // Sleep and wake again, to catch a wakeup path that only works once.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::atomic<int> second{0};
    for (int i = 0; i < 200; ++i) {
        (void)runtime.submit([&second] { second.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();
    TB_CHECK_EQ(second.load(), 200);
}

TB_TEST("a single-worker runtime does not attempt to steal from itself") {
    RuntimeConfig config;
    config.worker_count = 1;
    ThunderboltRuntime runtime(config);

    std::atomic<int> ran{0};
    for (int i = 0; i < 100; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    TB_CHECK_EQ(ran.load(), 100);
    TB_CHECK_EQ(runtime.stats().steals_succeeded, 0u);
}

TB_TEST("affinity is off by default and reported honestly when requested") {
    // S17: pinning must not be silently on, and a request the OS refused must not
    // be reported as a pinned run - a benchmark row would otherwise describe a
    // configuration that never existed.
    {
        RuntimeConfig config;
        config.worker_count = 2;
        ThunderboltRuntime runtime(config);
        TB_CHECK(config.affinity == AffinityMode::Disabled);
        TB_CHECK_EQ(runtime.pinned_worker_count(), 0u);
    }
    {
        RuntimeConfig config;
        config.worker_count = 2;
        config.affinity     = AffinityMode::TopologyAware;
        ThunderboltRuntime runtime(config);

        std::atomic<int> ran{0};
        for (int i = 0; i < 50; ++i) {
            (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
        runtime.wait_all();
        TB_CHECK_EQ(ran.load(), 50);

        // Never more than the workers that exist; may be fewer if the OS refused.
        TB_CHECK(runtime.pinned_worker_count() <= runtime.worker_count());
    }
}

TB_TEST("stats account for every executed task") {
    RuntimeConfig config;
    config.worker_count = 4;
    ThunderboltRuntime runtime(config);

    constexpr int kTasks = 5000;
    std::atomic<int> ran{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    const auto stats = runtime.stats();
    TB_CHECK_EQ(ran.load(), kTasks);
    // Counted per worker; must agree with the base runtime's own completion count.
    TB_CHECK_EQ(stats.tasks_executed, runtime.completed_task_count());
}
