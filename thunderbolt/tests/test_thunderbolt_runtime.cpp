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
#include <cstdio>
#include <cstdlib>
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

// ---- Phase I Stage 3: in-place submission ------------------------------------

namespace {

// Copy can throw, move cannot: the shape that makes submitting a const lvalue
// fail AFTER the pool slot was reserved.
struct ThrowingCopy {
    int* counter;
    bool throws;
    ThrowingCopy(int* c, bool t) : counter(c), throws(t) {}
    ThrowingCopy(const ThrowingCopy& other) : counter(other.counter), throws(other.throws) {
        if (throws) {
            throw 42;  // conditional, so the compiler cannot prove the code after it dead
        }
    }
    ThrowingCopy(ThrowingCopy&& other) noexcept : counter(other.counter), throws(false) {}
    void operator()() const { ++*counter; }
};

} // namespace

TB_TEST("in-place submit runs lvalue, const and rvalue callables exactly once") {
    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 4096;
    config.optimizations = thunderbolt::kOptInPlaceSubmit | thunderbolt::kOptThreadCache;
    ThunderboltRuntime runtime(config);

    std::atomic<int> ran{0};
    auto             lvalue = [&ran] { ran.fetch_add(1); };
    const auto       konst  = [&ran] { ran.fetch_add(10); };
    (void)runtime.submit(lvalue);
    (void)runtime.submit(konst);
    (void)runtime.submit([&ran](TaskContext&) { ran.fetch_add(100); });
    runtime.wait_all();
    TB_CHECK_EQ(ran.load(), 111);
}

TB_TEST("in-place submit whose callable throws on copy leaks neither slot nor count") {
    RuntimeConfig config;
    config.worker_count  = 2;
    config.task_capacity = 4096;
    config.optimizations = thunderbolt::kOptInPlaceSubmit;
    ThunderboltRuntime runtime(config);

    int              counter = 0;
    const ThrowingCopy callable(&counter, true);
    for (int i = 0; i < 5000; ++i) {  // more than the pool holds: a leak would exhaust it
        bool threw = false;
        try {
            (void)runtime.submit(callable);
        } catch (int) {
            threw = true;
        }
        TB_CHECK(threw);
    }
    runtime.wait_all();  // would hang if outstanding_ had been left incremented
    TB_CHECK_EQ(runtime.pool_acquire_count(), 5000u);
    TB_CHECK_EQ(runtime.inline_execution_count(), 0u);
    TB_CHECK_EQ(counter, 0);
}

TB_TEST("in-place submit degrades to inline execution when the pool is exhausted") {
    RuntimeConfig config;
    config.worker_count  = 1;
    config.task_capacity = 4;
    config.optimizations = thunderbolt::kOptInPlaceSubmit;
    ThunderboltRuntime runtime(config);

    std::atomic<bool> gate{false};
    std::atomic<int>  ran{0};
    for (int i = 0; i < 4; ++i) {
        (void)runtime.submit([&gate, &ran] {
            while (!gate.load()) { std::this_thread::yield(); }
            ran.fetch_add(1);
        });
    }
    (void)runtime.submit([&ran] { ran.fetch_add(1); });  // pool is full: runs inline
    TB_CHECK_EQ(runtime.inline_execution_count(), 1u);
    gate.store(true);
    runtime.wait_all();
    TB_CHECK_EQ(ran.load(), 5);
}

// ---------------------------------------------------------------------------
// Successor registration racing completion.
//
// The project's one real deadlock lived in this exact spot: a dependent that
// registered onto a task that had already drained its successor list was never
// notified. Here a predecessor is submitted and, at the same instant, several
// threads call submit_after() on it - so registrations land before, during and
// after its completion (and, with a recycled slot, after its generation moved).
// Every dependent must run exactly once. A lost successor shows up as a counter
// that never reaches its target, so the wait is bounded and a miss aborts rather
// than hanging the suite.
namespace {

void successor_race(std::uint32_t optimizations, int rounds) {
    constexpr int kRacers = 3;

    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 4096;
    config.optimizations = optimizations;
    ThunderboltRuntime runtime(config);

    std::atomic<long> dependents_ran{0};
    std::atomic<int>  arrived{0};
    std::atomic<int>  round_go{-1};
    std::atomic<bool> stop{false};

    std::vector<std::thread> racers;
    std::vector<TaskHandle> handles(static_cast<std::size_t>(rounds));

    for (int r = 0; r < kRacers; ++r) {
        racers.emplace_back([&] {
            for (int round = 0; round < rounds; ++round) {
                while (round_go.load(std::memory_order_acquire) < round) {
                    if (stop.load(std::memory_order_relaxed)) return;
                }
                const TaskHandle target = handles[static_cast<std::size_t>(round)];
                // Burn a slot or two so the registration lands at a varying offset
                // from the predecessor's completion.
                // (A signal fence, not a volatile counter: ++ on a volatile is
                // deprecated in C++20 and fails GCC/Clang builds under -Werror.)
                for (int spin = 0; spin < (round & 63); ++spin) {
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                }
                (void)runtime.submit_after({target}, [&dependents_ran] {
                    dependents_ran.fetch_add(1, std::memory_order_relaxed);
                });
                arrived.fetch_add(1, std::memory_order_release);
            }
        });
    }

    for (int round = 0; round < rounds; ++round) {
        arrived.store(0, std::memory_order_relaxed);
        handles[static_cast<std::size_t>(round)] = runtime.submit([] {});
        round_go.store(round, std::memory_order_release);
        // The predecessor is already runnable on a worker while the racers register.
        while (arrived.load(std::memory_order_acquire) < kRacers) {}
    }
    for (auto& t : racers) t.join();

    const long expected = static_cast<long>(rounds) * kRacers;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (dependents_ran.load() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool all_ran = dependents_ran.load() == expected;
    if (!all_ran) {
        std::fprintf(stderr, "LOST SUCCESSOR: %ld of %ld dependents ran\n", dependents_ran.load(),
                     expected);
        std::fflush(stderr);
        std::abort();  // a stuck runtime would hang its destructor too
    }
    runtime.wait_all();
    TB_CHECK_EQ(dependents_ran.load(), expected);
}

} // namespace

TB_TEST("submit_after racing the predecessor's completion loses no dependent") {
    successor_race(thunderbolt::kOptThreadCache | thunderbolt::kOptInPlaceSubmit, 4000);
}
