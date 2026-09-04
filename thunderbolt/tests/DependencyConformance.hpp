// Dependency behaviour every ITaskRuntime must exhibit identically.
//
// Like RuntimeConformance.hpp, these run against BOTH runtimes. Dependency
// resolution lives in RuntimeBase precisely so the two cannot diverge, and this
// suite is what would catch it if that ever stopped being true.
//
// A note on what these tests check. "The dependent produced the right answer" is
// weak evidence: with a fast predecessor and a slow scheduler, an UNORDERED
// implementation passes it most of the time. So each test below makes the
// predecessor slow, or the ordering explicitly observable, so that a missing
// dependency edge fails rather than merely becoming unlikely.
#pragma once

#include "TestHarness.hpp"

#include <thunderbolt/api/ITaskRuntime.hpp>
#include <thunderbolt/api/RuntimeConfig.hpp>
#include <thunderbolt/api/TaskContext.hpp>
#include <thunderbolt/api/TaskHandle.hpp>
#include <thunderbolt/core/taskgraph/TaskGraph.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace thunderbolt::conformance {

// Busy work long enough that an unordered implementation would reliably run the
// dependent first. Sleeping would be simpler but yields the core, which lets a
// broken implementation look correct.
inline void spin_for(std::chrono::microseconds duration) {
    const auto   deadline = std::chrono::steady_clock::now() + duration;
    volatile int sink     = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 100; ++i) {
            sink += i;
        }
    }
}

inline void spin_briefly() { spin_for(std::chrono::milliseconds(5)); }

template <typename R>
void dependent_runs_after_its_predecessor() {
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    std::atomic<bool> predecessor_done{false};
    std::atomic<bool> observed_ordering{false};

    TaskHandle first = runtime.submit([&predecessor_done] {
        spin_briefly();
        predecessor_done.store(true, std::memory_order_release);
    });

    TaskHandle second = runtime.submit_after({first}, [&predecessor_done, &observed_ordering] {
        observed_ordering.store(predecessor_done.load(std::memory_order_acquire),
                                std::memory_order_release);
    });

    runtime.wait(second);
    TB_CHECK(observed_ordering.load());
}

template <typename R>
void a_task_with_many_dependencies_waits_for_all() {
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    constexpr int    kPredecessors = 16;
    std::atomic<int> finished{0};
    std::atomic<int> seen_at_join{-1};

    std::vector<TaskHandle> predecessors;
    predecessors.reserve(kPredecessors);
    for (int i = 0; i < kPredecessors; ++i) {
        predecessors.push_back(runtime.submit([&finished] {
            spin_briefly();
            finished.fetch_add(1, std::memory_order_acq_rel);
        }));
    }

    TaskHandle join = runtime.submit_after(
        predecessors, TaskDesc{TaskFunction{[&finished, &seen_at_join] {
            seen_at_join.store(finished.load(std::memory_order_acquire), std::memory_order_release);
        }}});

    runtime.wait(join);
    // Every predecessor must have completed before the join body ran. Anything
    // less means an edge was dropped.
    TB_CHECK_EQ(seen_at_join.load(), kPredecessors);
}

template <typename R>
void a_diamond_orders_correctly() {
    // A -> {B, C} -> D. The classic shape: D must see both branches, and B and C
    // must both see A.
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    std::mutex               order_mutex;
    std::vector<const char*> order;
    auto record = [&order_mutex, &order](const char* label) {
        std::lock_guard lock(order_mutex);
        order.push_back(label);
    };

    TaskHandle a = runtime.submit([&record] {
        spin_briefly();
        record("A");
    });
    TaskHandle b = runtime.submit_after({a}, [&record] { record("B"); });
    TaskHandle c = runtime.submit_after({a}, [&record] { record("C"); });
    TaskHandle d = runtime.submit_after({b, c}, [&record] { record("D"); });

    runtime.wait(d);

    TB_CHECK_EQ(order.size(), 4u);
    TB_CHECK(order.front() == std::string_view("A"));  // A strictly first
    TB_CHECK(order.back() == std::string_view("D"));   // D strictly last
}

template <typename R>
void a_long_chain_runs_in_order() {
    // Depth rather than width. Each link records its position, so a link that ran
    // early is visible rather than merely producing a wrong total.
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    constexpr int    kLength = 200;
    std::atomic<int> counter{0};
    std::atomic<int> violations{0};

    TaskHandle previous;
    for (int i = 0; i < kLength; ++i) {
        auto body = [&counter, &violations, i] {
            // Each link must observe exactly i completed predecessors.
            if (counter.fetch_add(1, std::memory_order_acq_rel) != i) {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
        };
        previous = (i == 0) ? runtime.submit(body) : runtime.submit_after({previous}, body);
    }

    runtime.wait(previous);
    TB_CHECK_EQ(counter.load(), kLength);
    TB_CHECK_EQ(violations.load(), 0);
}

template <typename R>
void a_wide_fan_out_and_in_completes() {
    RuntimeConfig config;
    config.worker_count  = 4;
    config.task_capacity = 8192;
    R runtime(config);

    // Each middle task does a little real work. Without it they complete so fast
    // that the join runs last by accident, and the test passes even when
    // dependencies are ignored entirely - which a mutation confirmed.
    constexpr int    kWidth = 200;
    std::atomic<int> ran{0};

    TaskHandle root = runtime.submit([] { spin_briefly(); });

    std::vector<TaskHandle> middle;
    middle.reserve(kWidth);
    for (int i = 0; i < kWidth; ++i) {
        middle.push_back(runtime.submit_after({root}, [&ran] {
            spin_for(std::chrono::microseconds(200));
            ran.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    std::atomic<int> seen{-1};
    TaskHandle       join = runtime.submit_after(
        middle, TaskDesc{TaskFunction{[&ran, &seen] {
            seen.store(ran.load(std::memory_order_acquire), std::memory_order_release);
        }}});

    runtime.wait(join);
    TB_CHECK_EQ(ran.load(), kWidth);
    TB_CHECK_EQ(seen.load(), kWidth);
}

template <typename R>
void dependencies_on_already_completed_tasks_are_satisfied() {
    // A handle whose task finished long ago - and whose pool slot has since been
    // recycled - must count as satisfied, not hang. This is the case where a
    // naive implementation deadlocks: the predecessor can never signal again.
    RuntimeConfig config;
    config.worker_count = 2;
    R runtime(config);

    TaskHandle first = runtime.submit([] {});
    runtime.wait(first);

    // Churn the pool so the slot behind `first` is reused and the handle is stale.
    for (int i = 0; i < 100; ++i) {
        (void)runtime.submit([] {});
    }
    runtime.wait_all();

    std::atomic<bool> ran{false};
    TaskHandle second = runtime.submit_after({first}, [&ran] { ran.store(true); });
    runtime.wait(second);
    TB_CHECK(ran.load());
}

template <typename R>
void submit_after_with_no_dependencies_behaves_like_submit() {
    RuntimeConfig config;
    config.worker_count = 2;
    R runtime(config);

    std::atomic<bool> ran{false};
    TaskHandle handle = runtime.submit_after(TaskDesc{TaskFunction{[&ran] { ran.store(true); }}},
                                             nullptr, 0);
    runtime.wait(handle);
    TB_CHECK(ran.load());
}

template <typename R>
void dependencies_spill_past_the_inline_successor_budget() {
    // A task's inline successor storage holds six entries; beyond that the list
    // spills to the heap. The spill path is the bug-prone one, so cross it.
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    constexpr int kDependents = 64;

    std::atomic<bool> predecessor_done{false};
    std::atomic<int>  ordering_violations{0};
    std::atomic<int>  ran{0};

    TaskHandle root = runtime.submit([&predecessor_done] {
        spin_briefly();
        predecessor_done.store(true, std::memory_order_release);
    });

    std::vector<TaskHandle> dependents;
    dependents.reserve(kDependents);
    for (int i = 0; i < kDependents; ++i) {
        dependents.push_back(runtime.submit_after({root}, [&] {
            if (!predecessor_done.load(std::memory_order_acquire)) {
                ordering_violations.fetch_add(1, std::memory_order_relaxed);
            }
            ran.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (TaskHandle handle : dependents) {
        runtime.wait(handle);
    }

    TB_CHECK_EQ(ran.load(), kDependents);
    TB_CHECK_EQ(ordering_violations.load(), 0);  // every one of them waited
}

template <typename R>
void dependencies_declared_from_inside_a_task() {
    // Building a sub-graph from within a running task is what a frame graph
    // actually does. It also exercises the registration path concurrently with
    // other workers completing tasks.
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    std::atomic<int> ordering_violations{0};

    TaskHandle outer = runtime.submit([&runtime, &ordering_violations](TaskContext&) {
        std::atomic<bool> inner_done{false};
        TaskHandle        first = runtime.submit([&inner_done] {
            spin_briefly();
            inner_done.store(true, std::memory_order_release);
        });
        TaskHandle second = runtime.submit_after({first}, [&inner_done, &ordering_violations] {
            if (!inner_done.load(std::memory_order_acquire)) {
                ordering_violations.fetch_add(1, std::memory_order_relaxed);
            }
        });
        runtime.wait(second);
    });

    runtime.wait(outer);
    TB_CHECK_EQ(ordering_violations.load(), 0);
}

template <typename R>
void task_graph_submits_and_orders() {
    RuntimeConfig config;
    config.worker_count = 4;
    R runtime(config);

    std::mutex               order_mutex;
    std::vector<const char*> order;
    auto record = [&order_mutex, &order](const char* label) {
        std::lock_guard lock(order_mutex);
        order.push_back(label);
    };

    TaskGraph graph;
    const auto input   = graph.add([&record] { spin_briefly(); record("input"); });
    const auto physics = graph.add([&record] { record("physics"); });
    const auto ai      = graph.add([&record] { record("ai"); });
    const auto render  = graph.add([&record] { record("render"); });

    graph.add_edge(input, physics);
    graph.add_edge(input, ai);
    graph.add_edge(physics, render);
    graph.add_edge(ai, render);

    TB_CHECK(graph.is_acyclic());
    TB_CHECK(graph.submit_and_wait(runtime));

    TB_CHECK_EQ(order.size(), 4u);
    TB_CHECK(order.front() == std::string_view("input"));
    TB_CHECK(order.back() == std::string_view("render"));
}

template <typename R>
void task_graph_refuses_a_cycle_instead_of_deadlocking() {
    RuntimeConfig config;
    config.worker_count = 2;
    R runtime(config);

    TaskGraph  graph;
    const auto a = graph.add([] {});
    const auto b = graph.add([] {});
    const auto c = graph.add([] {});
    graph.add_edge(a, b);
    graph.add_edge(b, c);
    graph.add_edge(c, a);  // closes the loop

    TB_CHECK(!graph.is_acyclic());
    TB_CHECK(graph.topological_order().empty());
    // Submitting would deadlock - every node waiting on a node that is waiting.
    // Refusing is the only safe answer.
    TB_CHECK(!graph.submit_and_wait(runtime, TaskGraph::CyclePolicy::RefuseQuietly));
}

} // namespace thunderbolt::conformance

#define TB_DEPENDENCY_CONFORMANCE_SUITE(RuntimeT, Label)                                         \
    TB_TEST(Label " a dependent runs after its predecessor") {                                   \
        ::thunderbolt::conformance::dependent_runs_after_its_predecessor<RuntimeT>();            \
    }                                                                                            \
    TB_TEST(Label " a task with many dependencies waits for all of them") {                      \
        ::thunderbolt::conformance::a_task_with_many_dependencies_waits_for_all<RuntimeT>();     \
    }                                                                                            \
    TB_TEST(Label " a diamond graph orders correctly") {                                         \
        ::thunderbolt::conformance::a_diamond_orders_correctly<RuntimeT>();                      \
    }                                                                                            \
    TB_TEST(Label " a long chain runs strictly in order") {                                      \
        ::thunderbolt::conformance::a_long_chain_runs_in_order<RuntimeT>();                      \
    }                                                                                            \
    TB_TEST(Label " a wide fan-out and fan-in completes") {                                      \
        ::thunderbolt::conformance::a_wide_fan_out_and_in_completes<RuntimeT>();                 \
    }                                                                                            \
    TB_TEST(Label " a dependency on an already-completed task is satisfied") {                   \
        ::thunderbolt::conformance::dependencies_on_already_completed_tasks_are_satisfied<       \
            RuntimeT>();                                                                         \
    }                                                                                            \
    TB_TEST(Label " submit_after with no dependencies behaves like submit") {                    \
        ::thunderbolt::conformance::submit_after_with_no_dependencies_behaves_like_submit<       \
            RuntimeT>();                                                                         \
    }                                                                                            \
    TB_TEST(Label " successor lists spill past their inline budget correctly") {                 \
        ::thunderbolt::conformance::dependencies_spill_past_the_inline_successor_budget<         \
            RuntimeT>();                                                                         \
    }                                                                                            \
    TB_TEST(Label " dependencies can be declared from inside a running task") {                  \
        ::thunderbolt::conformance::dependencies_declared_from_inside_a_task<RuntimeT>();        \
    }                                                                                            \
    TB_TEST(Label " TaskGraph submits a frame graph in order") {                                 \
        ::thunderbolt::conformance::task_graph_submits_and_orders<RuntimeT>();                   \
    }                                                                                            \
    TB_TEST(Label " TaskGraph refuses a cycle instead of deadlocking") {                         \
        ::thunderbolt::conformance::task_graph_refuses_a_cycle_instead_of_deadlocking<           \
            RuntimeT>();                                                                         \
    }
