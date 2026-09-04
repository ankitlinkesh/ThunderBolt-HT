// TaskGraph construction and validation, with no runtime involved.
//
// The graph is a pure construction-time structure: no threads, no atomics. That
// is deliberate - it is the one place in the project where dependency cycles can
// be expressed, so validating it is a plain algorithmic problem rather than a
// concurrent one.
#include "TestHarness.hpp"

#include <thunderbolt/core/taskgraph/TaskGraph.hpp>

using thunderbolt::TaskGraph;

TB_TEST("an empty graph is acyclic and submits nothing") {
    TaskGraph graph;
    TB_CHECK_EQ(graph.node_count(), 0u);
    TB_CHECK(graph.is_acyclic());
    TB_CHECK(graph.topological_order().empty());
}

TB_TEST("a graph with no edges orders every node") {
    TaskGraph graph;
    for (int i = 0; i < 5; ++i) {
        (void)graph.add([] {});
    }
    TB_CHECK(graph.is_acyclic());
    TB_CHECK_EQ(graph.topological_order().size(), 5u);
}

TB_TEST("topological order places predecessors before successors") {
    TaskGraph  graph;
    const auto a = graph.add([] {});
    const auto b = graph.add([] {});
    const auto c = graph.add([] {});
    graph.add_edge(a, b);
    graph.add_edge(b, c);

    const auto order = graph.topological_order();
    TB_CHECK_EQ(order.size(), 3u);

    std::size_t position_a = 0, position_b = 0, position_c = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == a) position_a = i;
        if (order[i] == b) position_b = i;
        if (order[i] == c) position_c = i;
    }
    TB_CHECK(position_a < position_b);
    TB_CHECK(position_b < position_c);
}

TB_TEST("a two-node cycle is detected") {
    TaskGraph  graph;
    const auto a = graph.add([] {});
    const auto b = graph.add([] {});
    graph.add_edge(a, b);
    graph.add_edge(b, a);
    TB_CHECK(!graph.is_acyclic());
    TB_CHECK(graph.topological_order().empty());
}

TB_TEST("a cycle buried among acyclic nodes is still detected") {
    // Kahn's algorithm orders everything it can reach and stops. The nodes left
    // over are exactly the ones trapped in the cycle - so a cycle in one corner of
    // an otherwise-fine graph must not be masked by the parts that ordered
    // successfully.
    TaskGraph  graph;
    const auto free_a = graph.add([] {});
    const auto free_b = graph.add([] {});
    graph.add_edge(free_a, free_b);

    const auto x = graph.add([] {});
    const auto y = graph.add([] {});
    const auto z = graph.add([] {});
    graph.add_edge(x, y);
    graph.add_edge(y, z);
    graph.add_edge(z, x);

    TB_CHECK(!graph.is_acyclic());
}

TB_TEST("duplicate edges collapse to a single ordering constraint") {
    // A duplicate edge increments the predecessor count twice, so the decrement
    // side must match it. If they disagreed, the node would either never become
    // ready or would become ready too early.
    TaskGraph  graph;
    const auto a = graph.add([] {});
    const auto b = graph.add([] {});
    graph.add_edge(a, b);
    graph.add_edge(a, b);

    TB_CHECK_EQ(graph.edge_count(), 2u);
    TB_CHECK(graph.is_acyclic());
    TB_CHECK_EQ(graph.topological_order().size(), 2u);
}

TB_TEST("a diamond is acyclic") {
    TaskGraph  graph;
    const auto a = graph.add([] {});
    const auto b = graph.add([] {});
    const auto c = graph.add([] {});
    const auto d = graph.add([] {});
    graph.add_edge(a, b);
    graph.add_edge(a, c);
    graph.add_edge(b, d);
    graph.add_edge(c, d);

    TB_CHECK(graph.is_acyclic());
    const auto order = graph.topological_order();
    TB_CHECK_EQ(order.size(), 4u);
    TB_CHECK_EQ(order.front(), a);
    TB_CHECK_EQ(order.back(), d);
}
