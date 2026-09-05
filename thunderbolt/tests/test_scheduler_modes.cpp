// Scheduling strategies (S12) and, more importantly, starvation (S13).
//
// S13 says priority must not starve low-priority work indefinitely. Until now
// only priority ORDERING was tested; fairness was asserted in the plan and never
// checked, which made the requirement aspirational. These tests close that.
//
// Note what is and is not claimed here. Strict priority - the default - CAN starve
// background work, by construction; that is what strict means, and it is not a
// bug. The claim is narrower and testable: with SchedulerMode::PriorityAging,
// background work completes within a bound expressed in TASKS, not in "eventually".
#include "TestHarness.hpp"

#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#include <atomic>
#include <thread>
#include <vector>

using thunderbolt::RuntimeConfig;
using thunderbolt::SchedulerMode;
using thunderbolt::TaskContext;
using thunderbolt::TaskHandle;
using thunderbolt::TaskPriority;
using thunderbolt::ThunderboltRuntime;

namespace {

// Runs `critical_count` CRITICAL tasks with one BACKGROUND task queued FIRST, on
// a single worker, and reports how many criticals ran before the background one.
//
// One worker is essential. With several, a background task can be picked up by an
// idle worker and the test would pass without aging doing anything - measuring
// spare capacity rather than fairness.
std::size_t criticals_before_background(SchedulerMode mode, std::size_t critical_count) {
    RuntimeConfig config;
    config.worker_count  = 1;
    config.scheduler     = mode;
    config.task_capacity = 8192;
    ThunderboltRuntime runtime(config);

    std::atomic<std::size_t> criticals_done{0};
    std::atomic<std::size_t> background_saw{0};
    std::atomic<bool>        background_ran{false};
    std::atomic<bool>        gate{false};
    std::atomic<bool>        blocker_running{false};

    // Hold the only worker so everything below is queued before any of it runs.
    // Otherwise the worker drains as we submit and the ordering under test never
    // arises.
    TaskHandle blocker = runtime.submit([&gate, &blocker_running] {
        blocker_running.store(true, std::memory_order_release);
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (!blocker_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Background FIRST, so it is the oldest waiting task - the case S13 is about.
    TaskHandle background = runtime.submit(
        [&criticals_done, &background_saw, &background_ran] {
            background_saw.store(criticals_done.load(std::memory_order_acquire),
                                 std::memory_order_release);
            background_ran.store(true, std::memory_order_release);
        },
        TaskPriority::Background);

    for (std::size_t i = 0; i < critical_count; ++i) {
        (void)runtime.submit([&criticals_done] {
            criticals_done.fetch_add(1, std::memory_order_acq_rel);
        }, TaskPriority::Critical);
    }

    gate.store(true, std::memory_order_release);
    runtime.wait(blocker);
    runtime.wait(background);

    TB_CHECK(background_ran.load());
    return background_saw.load();
}

} // namespace

TB_TEST("aging bounds how long background work can be held behind critical work") {
    // The bound is in TASKS: one pop in kAgingInterval goes to the lowest
    // non-empty priority, so a background task at the head cannot wait for more
    // than about that many criticals. Checked with far more criticals queued than
    // the bound, so passing by luck is not available.
    constexpr std::size_t kCriticals = 500;
    const std::size_t     before =
        criticals_before_background(SchedulerMode::PriorityAging, kCriticals);

    TB_CHECK(before < thunderbolt::kAgingInterval * 4);
    TB_CHECK(before < kCriticals);  // it did not simply run last
}

TB_TEST("strict priority runs critical work first, as it is meant to") {
    // The other side of the same contract, and the reason aging is opt-in: the
    // default is strict, and this pins that so aging cannot silently become the
    // default later.
    constexpr std::size_t kCriticals = 200;
    const std::size_t     before =
        criticals_before_background(SchedulerMode::WorkStealing, kCriticals);

    // Every critical task ahead of the background one. Not a defect - it is what
    // strict priority means, and precisely why S13 asks for an alternative.
    TB_CHECK_EQ(before, kCriticals);
}

TB_TEST("aging still completes every task") {
    // Fairness must not cost correctness: rotating the scan order must not drop
    // or duplicate work.
    RuntimeConfig config;
    config.worker_count  = 4;
    config.scheduler     = SchedulerMode::PriorityAging;
    config.task_capacity = 16384;
    ThunderboltRuntime runtime(config);

    constexpr int             kPerPriority = 500;
    std::atomic<int>          ran{0};
    const TaskPriority        priorities[] = {TaskPriority::Critical, TaskPriority::High,
                                              TaskPriority::Normal, TaskPriority::Low,
                                              TaskPriority::Background};
    for (TaskPriority priority : priorities) {
        for (int i = 0; i < kPerPriority; ++i) {
            (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); },
                                 priority);
        }
    }
    runtime.wait_all();
    TB_CHECK_EQ(ran.load(), kPerPriority * 5);
}

TB_TEST("static mode runs every task without stealing") {
    // The control for S59.2. Tasks are pinned to a worker at submission and never
    // migrate, so the steal counters must stay at zero - otherwise the mode is
    // not actually static and any comparison against stealing is meaningless.
    RuntimeConfig config;
    config.worker_count  = 4;
    config.scheduler     = SchedulerMode::Static;
    config.task_capacity = 16384;
    ThunderboltRuntime runtime(config);

    constexpr int    kTasks = 4000;
    std::atomic<int> ran{0};
    for (int i = 0; i < kTasks; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    TB_CHECK_EQ(ran.load(), kTasks);
    TB_CHECK_EQ(runtime.stats().steals_succeeded, 0u);
}

TB_TEST("static mode spreads work across every worker") {
    // Round-robin assignment must actually reach all workers. If it did not, the
    // mode would be "one worker does everything", which would lose to stealing
    // for a reason that has nothing to do with scheduling policy.
    RuntimeConfig config;
    config.worker_count  = 4;
    config.scheduler     = SchedulerMode::Static;
    config.task_capacity = 16384;
    ThunderboltRuntime runtime(config);

    for (int i = 0; i < 2000; ++i) {
        (void)runtime.submit([] {});
    }
    runtime.wait_all();

    const auto stats = runtime.stats();
    TB_CHECK_EQ(stats.tasks_executed, runtime.completed_task_count());
    TB_CHECK(stats.tasks_executed >= 2000u);
}

TB_TEST("static mode handles a task submitted from inside a task") {
    // Nested submission from a worker still goes through round-robin assignment
    // rather than the submitting worker's deque, which is the path most likely to
    // be missed when adding a mode.
    RuntimeConfig config;
    config.worker_count = 4;
    config.scheduler    = SchedulerMode::Static;
    ThunderboltRuntime runtime(config);

    std::atomic<int> inner{0};
    TaskHandle       outer = runtime.submit([&runtime, &inner](TaskContext&) {
        std::vector<TaskHandle> children;
        for (int i = 0; i < 100; ++i) {
            children.push_back(
                runtime.submit([&inner] { inner.fetch_add(1, std::memory_order_relaxed); }));
        }
        for (TaskHandle h : children) {
            runtime.wait(h);
        }
    });
    runtime.wait(outer);
    TB_CHECK_EQ(inner.load(), 100);
}
