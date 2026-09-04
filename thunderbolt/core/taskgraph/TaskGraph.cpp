#include <thunderbolt/core/taskgraph/TaskGraph.hpp>

#include <cassert>
#include <deque>

namespace thunderbolt {

TaskGraph::NodeId TaskGraph::add(TaskDesc desc) {
    const NodeId id = static_cast<NodeId>(nodes_.size());
    Node         node;
    node.desc = std::move(desc);
    nodes_.push_back(std::move(node));
    return id;
}

void TaskGraph::add_edge(NodeId before, NodeId after) {
    assert(before < nodes_.size() && "edge references a node that does not exist");
    assert(after < nodes_.size() && "edge references a node that does not exist");
    if (before >= nodes_.size() || after >= nodes_.size()) {
        return;
    }

    // A self-edge is a one-node cycle. Rejecting it here gives a precise
    // diagnostic at the point of the mistake, rather than a "graph is cyclic"
    // failure later that says nothing about which edge caused it.
    assert(before != after && "a task cannot depend on itself");
    if (before == after) {
        return;
    }

    nodes_[after].predecessors.push_back(before);
    nodes_[before].successors.push_back(after);
    ++edge_count_;
}

std::vector<TaskGraph::NodeId> TaskGraph::topological_order() const {
    // Kahn's algorithm. Chosen over DFS colouring because it produces the
    // submission order and the cycle verdict in one pass: if fewer nodes come out
    // than went in, the remainder are exactly the ones trapped in a cycle.
    const std::size_t count = nodes_.size();

    std::vector<std::uint32_t> remaining_predecessors(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        remaining_predecessors[i] = static_cast<std::uint32_t>(nodes_[i].predecessors.size());
    }

    std::deque<NodeId> ready;
    for (std::size_t i = 0; i < count; ++i) {
        if (remaining_predecessors[i] == 0) {
            ready.push_back(static_cast<NodeId>(i));
        }
    }

    std::vector<NodeId> order;
    order.reserve(count);

    while (!ready.empty()) {
        const NodeId id = ready.front();
        ready.pop_front();
        order.push_back(id);

        for (NodeId successor : nodes_[id].successors) {
            // Duplicate edges decrement more than once, which is correct: they
            // were counted more than once in the predecessor list too.
            if (--remaining_predecessors[successor] == 0) {
                ready.push_back(successor);
            }
        }
    }

    if (order.size() != count) {
        return {};  // cyclic
    }
    return order;
}

bool TaskGraph::is_acyclic() const { return nodes_.empty() || !topological_order().empty(); }

std::vector<TaskHandle> TaskGraph::submit(ITaskRuntime& runtime, CyclePolicy policy) {
    if (nodes_.empty()) {
        return {};
    }

    const std::vector<NodeId> order = topological_order();
    if (order.empty()) {
        // Submitting a cyclic graph would deadlock: every node would sit waiting
        // for a predecessor that is itself waiting. Refusing is the only safe
        // answer, and by default this is a hard failure rather than a quiet one.
        assert((policy == CyclePolicy::RefuseQuietly) &&
               "TaskGraph contains a dependency cycle and cannot be submitted");
        (void)policy;
        return {};
    }

    std::vector<TaskHandle> handles(nodes_.size());
    std::vector<TaskHandle> dependency_handles;

    for (NodeId id : order) {
        Node& node = nodes_[id];

        dependency_handles.clear();
        dependency_handles.reserve(node.predecessors.size());
        for (NodeId predecessor : node.predecessors) {
            // Topological order guarantees every predecessor was submitted first,
            // so its handle is already populated. A predecessor that has ALREADY
            // completed by now is fine: submit_after treats a stale handle as a
            // satisfied dependency.
            dependency_handles.push_back(handles[predecessor]);
        }

        handles[id] = runtime.submit_after(std::move(node.desc), dependency_handles.data(),
                                           dependency_handles.size());
    }

    return handles;
}

bool TaskGraph::submit_and_wait(ITaskRuntime& runtime, CyclePolicy policy) {
    const std::vector<TaskHandle> handles = submit(runtime, policy);
    if (handles.empty() && !nodes_.empty()) {
        return false;  // rejected as cyclic
    }
    for (TaskHandle handle : handles) {
        runtime.wait(handle);
    }
    return true;
}

} // namespace thunderbolt
