#include <catch2/catch.hpp>
#include "needle/engine/resume_validator.h"
#include "needle/model/graph.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/validation/diagnostic.h"

using namespace needle;

namespace {

Graph make_basic_graph() {
    std::vector<Node> nodes;
    { Node n; n.id = "start"; n.type = NodeType::START; nodes.push_back(std::move(n)); }
    { Node n; n.id = "work"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "exit"; n.type = NodeType::EXIT; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "work"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "work"; e.to = "exit"; edges.push_back(std::move(e)); }

    return Graph::make("test", std::move(nodes), std::move(edges));
}

Checkpoint make_matching_checkpoint(const Graph& graph) {
    Checkpoint cp;
    cp.timestamp = "2026-03-18T10:00:00Z";
    cp.current_node = "work";
    cp.completed_nodes = {"start"};
    cp.graph_file = "test.dot";
    cp.graph_hash = ResumeValidator::compute_graph_hash(graph);
    return cp;
}

} // anonymous namespace

TEST_CASE("ResumeValidator: validate returns error when current_node missing from graph", "[resume_validator]") {
    Graph graph = make_basic_graph();

    Checkpoint cp;
    cp.current_node = "nonexistent_node";
    cp.completed_nodes = {"start"};
    cp.graph_hash = "";

    Diagnostics diags = ResumeValidator::validate(cp, graph);
    REQUIRE(diags.has_errors());

    auto errors = diags.errors();
    REQUIRE(errors.size() >= 1);
    bool found = false;
    for (const auto& e : errors) {
        if (e.code == "R001") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("ResumeValidator: validate returns error when completed_node missing from graph", "[resume_validator]") {
    Graph graph = make_basic_graph();

    Checkpoint cp;
    cp.current_node = "work";
    cp.completed_nodes = {"start", "deleted_step"};
    cp.graph_hash = "";

    Diagnostics diags = ResumeValidator::validate(cp, graph);
    REQUIRE(diags.has_errors());

    auto errors = diags.errors();
    bool found = false;
    for (const auto& e : errors) {
        if (e.code == "R002" && e.node_id == "deleted_step") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("ResumeValidator: validate returns warning on graph hash mismatch", "[resume_validator]") {
    Graph graph = make_basic_graph();

    Checkpoint cp;
    cp.current_node = "work";
    cp.completed_nodes = {"start"};
    cp.graph_hash = "totally_different_hash";

    Diagnostics diags = ResumeValidator::validate(cp, graph);
    REQUIRE_FALSE(diags.has_errors());
    REQUIRE(diags.has_warnings());

    bool found = false;
    for (const auto& d : diags.all()) {
        if (d.code == "R003") found = true;
    }
    REQUIRE(found);
}

TEST_CASE("ResumeValidator: validate returns no issues when checkpoint matches graph", "[resume_validator]") {
    Graph graph = make_basic_graph();
    Checkpoint cp = make_matching_checkpoint(graph);

    Diagnostics diags = ResumeValidator::validate(cp, graph);
    REQUIRE_FALSE(diags.has_errors());
    REQUIRE_FALSE(diags.has_warnings());
}

TEST_CASE("ResumeValidator: compute_graph_hash is deterministic", "[resume_validator]") {
    Graph graph = make_basic_graph();

    std::string hash1 = ResumeValidator::compute_graph_hash(graph);
    std::string hash2 = ResumeValidator::compute_graph_hash(graph);
    REQUIRE(hash1 == hash2);
    REQUIRE(hash1.size() == 16);  // 64-bit hex string
}

TEST_CASE("ResumeValidator: compute_graph_hash changes when nodes are added", "[resume_validator]") {
    Graph graph1 = make_basic_graph();
    std::string hash1 = ResumeValidator::compute_graph_hash(graph1);

    // Build a different graph with an extra node
    std::vector<Node> nodes;
    { Node n; n.id = "start"; n.type = NodeType::START; nodes.push_back(std::move(n)); }
    { Node n; n.id = "work"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "review"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "exit"; n.type = NodeType::EXIT; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "work"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "work"; e.to = "review"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "review"; e.to = "exit"; edges.push_back(std::move(e)); }

    Graph graph2 = Graph::make("test2", std::move(nodes), std::move(edges));
    std::string hash2 = ResumeValidator::compute_graph_hash(graph2);

    REQUIRE(hash1 != hash2);
}

TEST_CASE("ResumeValidator: compute_graph_hash changes when nodes are removed", "[resume_validator]") {
    Graph graph1 = make_basic_graph();
    std::string hash1 = ResumeValidator::compute_graph_hash(graph1);

    // Smaller graph: just start -> exit
    std::vector<Node> nodes;
    { Node n; n.id = "start"; n.type = NodeType::START; nodes.push_back(std::move(n)); }
    { Node n; n.id = "exit"; n.type = NodeType::EXIT; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "exit"; edges.push_back(std::move(e)); }

    Graph graph2 = Graph::make("test_small", std::move(nodes), std::move(edges));
    std::string hash2 = ResumeValidator::compute_graph_hash(graph2);

    REQUIRE(hash1 != hash2);
}

TEST_CASE("ResumeValidator: old checkpoint with empty graph_hash skips hash check", "[resume_validator]") {
    Graph graph = make_basic_graph();

    Checkpoint cp;
    cp.current_node = "work";
    cp.completed_nodes = {"start"};
    cp.graph_hash = "";  // Old checkpoint, no hash stored

    Diagnostics diags = ResumeValidator::validate(cp, graph);
    REQUIRE_FALSE(diags.has_errors());

    // Should not have R003 warning
    bool found_r003 = false;
    for (const auto& d : diags.all()) {
        if (d.code == "R003") found_r003 = true;
    }
    REQUIRE_FALSE(found_r003);
}

TEST_CASE("ResumeValidator: edits to unstarted nodes only — soft warning continues",
          "[resume_validator][n3]") {
    // Build a graph; complete `start` with its per-node hash recorded; mutate
    // the unstarted `work` node's prompt; new graph_hash will differ but the
    // completed `start` node's per-node hash still matches → soft R003 with
    // "only unstarted nodes affected" message and no error.
    Graph graph = make_basic_graph();

    Checkpoint cp;
    cp.timestamp = "2026-03-18T10:00:00Z";
    cp.current_node = "work";
    cp.completed_nodes = {"start"};
    cp.graph_hash = "deadbeef";  // pretend old hash differs from current
    const Node* start_node = graph.find_node("start");
    REQUIRE(start_node != nullptr);
    cp.completed_node_hashes["start"] = ResumeValidator::compute_node_hash(*start_node);

    Diagnostics diags = ResumeValidator::validate(cp, graph, /*strict=*/false);
    REQUIRE_FALSE(diags.has_errors());

    bool found_soft = false;
    for (const auto& d : diags.all()) {
        if (d.code == "R003" && d.message.find("only unstarted nodes") != std::string::npos) {
            found_soft = true;
        }
    }
    REQUIRE(found_soft);
}

TEST_CASE("ResumeValidator: completed-node edit warns; strict mode escalates to error",
          "[resume_validator][n3]") {
    Graph graph = make_basic_graph();

    Checkpoint cp;
    cp.timestamp = "2026-03-18T10:00:00Z";
    cp.current_node = "work";
    cp.completed_nodes = {"start"};
    cp.graph_hash = "deadbeef";
    cp.completed_node_hashes["start"] = "xx_pretend_old_node_hash";  // doesn't match current

    Diagnostics soft_diags = ResumeValidator::validate(cp, graph, /*strict=*/false);
    REQUIRE_FALSE(soft_diags.has_errors());
    bool warned = false;
    for (const auto& d : soft_diags.all()) {
        if (d.code == "R003" && d.message.find("completed node") != std::string::npos) {
            warned = true;
        }
    }
    REQUIRE(warned);

    Diagnostics strict_diags = ResumeValidator::validate(cp, graph, /*strict=*/true);
    REQUIRE(strict_diags.has_errors());
    bool errored = false;
    for (const auto& d : strict_diags.errors()) {
        if (d.code == "R003" && d.message.find("completed node") != std::string::npos) {
            errored = true;
        }
    }
    REQUIRE(errored);
}

TEST_CASE("ResumeValidator: compute_node_hash differs when prompt changes",
          "[resume_validator][n3]") {
    Node a;
    a.id = "n1";
    a.type = NodeType::CODERGEN;
    a.attrs.set("prompt", "do thing 1");
    std::string h1 = ResumeValidator::compute_node_hash(a);

    Node b;
    b.id = "n1";
    b.type = NodeType::CODERGEN;
    b.attrs.set("prompt", "do thing 2");
    std::string h2 = ResumeValidator::compute_node_hash(b);

    REQUIRE(h1 != h2);
}

TEST_CASE("ResumeValidator: compute_node_hash stable across attr-set order",
          "[resume_validator][n3]") {
    Node a;
    a.id = "n1";
    a.type = NodeType::CODERGEN;
    a.attrs.set("timeout", "90m");
    a.attrs.set("fidelity", "summary:high");
    a.attrs.set("prompt", "x");

    Node b;
    b.id = "n1";
    b.type = NodeType::CODERGEN;
    b.attrs.set("prompt", "x");
    b.attrs.set("fidelity", "summary:high");
    b.attrs.set("timeout", "90m");

    REQUIRE(ResumeValidator::compute_node_hash(a) == ResumeValidator::compute_node_hash(b));
}
