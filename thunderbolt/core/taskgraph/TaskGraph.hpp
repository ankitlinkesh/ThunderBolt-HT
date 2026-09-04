// Thunderbolt HT - offline task graph construction.
//
// The live runtime cannot express a dependency cycle: submit_after() only accepts
// handles that already exist, and a task's own handle does not exist until the
// call returns. That is a stronger guarantee than detecting cycles.
//
// This builder deliberately gives that up. A frame graph (S22, S52) is far easier
// to describe by naming nodes first and wiring them afterwards, which means edges
// between two already-created nodes - and therefore cycles. So this is where S62's
// cycle detection actually belongs, and it is a pure construction-time structure:
// no threads, no atomics, nothing to get wrong concurrently. Validation happens
// before a single task is submitted.
#pragma once

#include <thunderbolt/api/ITaskRuntime.hpp>
#include <thunderbolt/api/TaskDesc.hpp>
#include <thunderbolt/api/TaskHandle.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace thunderbolt {

class TaskGraph {
public:
    using NodeId = std::uint32_t;

    static constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

    // Adds a node. The body is stored and not run until submit().
    NodeId add(TaskDesc desc);

    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, TaskDesc>)
    NodeId add(F&& fn, TaskPriority priority = TaskPriority::Normal) {
        return add(TaskDesc{TaskFunction{std::forward<F>(fn)}, priority});
    }

    // Declares that `before` must complete before `after` starts.
    // Duplicate edges are allowed and collapse to one dependency.
    void add_edge(NodeId before, NodeId after);

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t edge_count() const noexcept { return edge_count_; }

    // True when the graph is a DAG. Runs Kahn's algorithm, which reports a cycle
    // as a natural side effect of failing to order every node.
    [[nodiscard]] bool is_acyclic() const;

    // Returns node ids in an order where every node follows its predecessors, or
    // an empty vector when the graph contains a cycle.
    [[nodiscard]] std::vector<NodeId> topological_order() const;

    // What to do about a cyclic graph.
    enum class CyclePolicy {
        // Assert in debug builds (S62: "in debug builds, fail loudly"), and
        // refuse in release. The default, because a cyclic frame graph is a
        // programming error rather than a runtime condition.
        FailLoudly,
        // Refuse quietly. For callers that legitimately expect a cycle - notably
        // the test that checks the refusal actually happens, which cannot use the
        // default without tripping the assert it is verifying.
        RefuseQuietly,
    };

    // Submits the whole graph, wiring each node's dependencies through
    // submit_after(). Returns handles indexed by NodeId, or an EMPTY vector when
    // the graph is cyclic. Submitting a cycle would deadlock - every node waiting
    // on a node that is itself waiting - so refusing is the only safe answer.
    [[nodiscard]] std::vector<TaskHandle> submit(ITaskRuntime& runtime,
                                                 CyclePolicy   policy = CyclePolicy::FailLoudly);

    // Submits and blocks until every node has completed.
    // Returns false when the graph was rejected as cyclic.
    bool submit_and_wait(ITaskRuntime& runtime, CyclePolicy policy = CyclePolicy::FailLoudly);

private:
    struct Node {
        TaskDesc            desc;
        std::vector<NodeId> predecessors;
        std::vector<NodeId> successors;
    };

    std::vector<Node> nodes_;
    std::size_t       edge_count_ = 0;
};

} // namespace thunderbolt
