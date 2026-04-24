#include <catch2/catch.hpp>
#include "needle/engine/subgraph_topology.h"
#include "needle/model/graph.h"

using namespace needle;

namespace {

// Helper: build a simple linear graph A -> B -> C -> FAN_IN
Graph make_linear_fan_in_graph() {
    std::vector<Node> nodes;
    { Node n; n.id = "A"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "B"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "C"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "fan_in"; n.type = NodeType::FAN_IN; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "A"; e.to = "B"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "B"; e.to = "C"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "C"; e.to = "fan_in"; edges.push_back(std::move(e)); }

    return Graph::make("linear_fan_in", std::move(nodes), std::move(edges));
}

// Helper: parallel graph with two branches converging at fan_in
Graph make_parallel_fan_in_graph() {
    std::vector<Node> nodes;
    { Node n; n.id = "par"; n.type = NodeType::PARALLEL; nodes.push_back(std::move(n)); }
    { Node n; n.id = "br_a"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "br_b"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "merge"; n.type = NodeType::FAN_IN; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "par"; e.to = "br_a"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "par"; e.to = "br_b"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "br_a"; e.to = "merge"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "br_b"; e.to = "merge"; edges.push_back(std::move(e)); }

    return Graph::make("parallel_fan_in", std::move(nodes), std::move(edges));
}

// M15: Branch with internal conditional edge (two paths to fan-in)
Graph make_branching_fan_in_graph() {
    std::vector<Node> nodes;
    { Node n; n.id = "par"; n.type = NodeType::PARALLEL; nodes.push_back(std::move(n)); }
    { Node n; n.id = "br_a"; n.type = NodeType::CONDITIONAL; nodes.push_back(std::move(n)); }
    { Node n; n.id = "a_left"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "a_right"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "br_b"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "merge"; n.type = NodeType::FAN_IN; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "par"; e.to = "br_a"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "par"; e.to = "br_b"; edges.push_back(std::move(e)); }
    // br_a has two outgoing edges (conditional)
    { Edge e; e.from = "br_a"; e.to = "a_left"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "br_a"; e.to = "a_right"; edges.push_back(std::move(e)); }
    // Both paths converge at merge
    { Edge e; e.from = "a_left"; e.to = "merge"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "a_right"; e.to = "merge"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "br_b"; e.to = "merge"; edges.push_back(std::move(e)); }

    return Graph::make("branching_fan_in", std::move(nodes), std::move(edges));
}

// Manager loop graph: manager -> work -> manager
Graph make_loop_body_graph() {
    std::vector<Node> nodes;
    { Node n; n.id = "manager"; n.type = NodeType::MANAGER_LOOP; nodes.push_back(std::move(n)); }
    { Node n; n.id = "work_a"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "work_b"; n.type = NodeType::CODERGEN; n.attrs.set("goal_gate", "true"); nodes.push_back(std::move(n)); }
    // An external node not part of the loop
    { Node n; n.id = "external"; n.type = NodeType::CODERGEN; n.attrs.set("goal_gate", "true"); nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "manager"; e.to = "work_a"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "work_a"; e.to = "work_b"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "work_b"; e.to = "manager"; edges.push_back(std::move(e)); }
    // external is separate, connected to nothing in the loop
    { Edge e; e.from = "manager"; e.to = "external"; edges.push_back(std::move(e)); }

    return Graph::make("loop_body", std::move(nodes), std::move(edges));
}

} // anonymous namespace

// ── collect_reachable tests ──────────────────────────────────────

TEST_CASE("SubgraphTopology: collect_reachable linear", "[subgraph_topology]") {
    Graph graph = make_linear_fan_in_graph();
    std::set<std::string> stop;
    stop.insert("fan_in");

    auto result = SubgraphTopology::collect_reachable(graph, "A", stop);
    REQUIRE(result.count("A"));
    REQUIRE(result.count("B"));
    REQUIRE(result.count("C"));
    REQUIRE_FALSE(result.count("fan_in"));  // stop node excluded
}

TEST_CASE("SubgraphTopology: collect_reachable with empty stop set", "[subgraph_topology]") {
    Graph graph = make_linear_fan_in_graph();
    std::set<std::string> stop;

    auto result = SubgraphTopology::collect_reachable(graph, "A", stop);
    REQUIRE(result.count("A"));
    REQUIRE(result.count("B"));
    REQUIRE(result.count("C"));
    REQUIRE(result.count("fan_in"));
}

// ── find_common_fan_in tests ─────────────────────────────────────

TEST_CASE("SubgraphTopology: find_common_fan_in simple parallel", "[subgraph_topology]") {
    Graph graph = make_parallel_fan_in_graph();
    std::vector<std::string> branches = {"br_a", "br_b"};

    std::string result = SubgraphTopology::find_common_fan_in(graph, branches);
    REQUIRE(result == "merge");
}

TEST_CASE("SubgraphTopology: find_common_fan_in with internal conditional (M15)", "[subgraph_topology]") {
    Graph graph = make_branching_fan_in_graph();
    std::vector<std::string> branches = {"br_a", "br_b"};

    std::string result = SubgraphTopology::find_common_fan_in(graph, branches);
    REQUIRE(result == "merge");
}

TEST_CASE("SubgraphTopology: find_common_fan_in no branches", "[subgraph_topology]") {
    Graph graph = make_parallel_fan_in_graph();
    std::vector<std::string> branches;

    std::string result = SubgraphTopology::find_common_fan_in(graph, branches);
    REQUIRE(result.empty());
}

// ── collect_loop_body tests ──────────────────────────────────────

TEST_CASE("SubgraphTopology: collect_loop_body includes body nodes", "[subgraph_topology]") {
    Graph graph = make_loop_body_graph();

    auto body = SubgraphTopology::collect_loop_body(graph, "manager", "work_a");
    REQUIRE(body.count("work_a"));
    REQUIRE(body.count("work_b"));
    REQUIRE_FALSE(body.count("manager"));  // loop-back target excluded
}

TEST_CASE("SubgraphTopology: collect_loop_body M12 — external node separate from loop body", "[subgraph_topology][M12]") {
    Graph graph = make_loop_body_graph();

    auto body = SubgraphTopology::collect_loop_body(graph, "manager", "work_a");
    // "external" is reachable from manager but is NOT reachable from work_a without going through manager
    // Actually, since manager has an edge to external, but we start from work_a, it depends on graph structure.
    // work_a -> work_b -> manager (stop), so external is not in the path from work_a
    REQUIRE(body.count("work_a"));
    REQUIRE(body.count("work_b"));
    // external is connected via manager -> external, not from work_a
    REQUIRE_FALSE(body.count("external"));
}
