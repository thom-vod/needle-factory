#include <catch2/catch.hpp>
#include "needle/validation/graph_validator.h"
#include "needle/validation/rules/single_start_node.h"
#include "needle/validation/rules/single_exit_node.h"
#include "needle/validation/rules/start_no_incoming.h"
#include "needle/validation/rules/exit_no_outgoing.h"
#include "needle/validation/rules/all_nodes_reachable.h"
#include "needle/validation/rules/valid_conditions.h"
#include "needle/validation/rules/parallel_fan_in_pairing.h"
#include "needle/validation/rules/no_self_loops.h"
#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "helpers/graph_fixtures.h"
#include <sstream>

using namespace needle;

// Helper to build a graph quickly
static Graph make_graph(std::vector<Node> nodes, std::vector<Edge> edges) {
    return Graph::make("test", std::move(nodes), std::move(edges));
}

static Node make_node(const std::string& id, NodeType type) {
    Node n;
    n.id = id;
    n.type = type;
    return n;
}

static Edge make_edge(const std::string& from, const std::string& to,
                      const std::string& condition = "") {
    Edge e;
    e.from = from;
    e.to = to;
    if (!condition.empty()) {
        e.attrs.set("condition", condition);
    }
    return e;
}

static Graph parse_graph_or_fail(const std::string& dot) {
    DotParser parser(dot);
    auto parsed = parser.parse();
    REQUIRE(parsed.ok());
    GraphBuilder builder;
    auto built = builder.build(parsed.value());
    REQUIRE(built.ok());
    return built.value();
}

// ====================== E001: SingleStartNode ======================

TEST_CASE("E001: valid graph has one START node", "[validator][E001]") {
    auto graph = fixtures::make_simple_graph();
    Diagnostics diags;
    SingleStartNodeRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("E001: no START node", "[validator][E001]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("a", NodeType::CODERGEN));
    nodes.push_back(make_node("b", NodeType::EXIT));
    auto graph = make_graph(std::move(nodes), {});

    Diagnostics diags;
    SingleStartNodeRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors().size() == 1);
    REQUIRE(diags.errors()[0].code == "E001");
}

TEST_CASE("E001: two START nodes", "[validator][E001]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("s1", NodeType::START));
    nodes.push_back(make_node("s2", NodeType::START));
    nodes.push_back(make_node("e", NodeType::EXIT));
    auto graph = make_graph(std::move(nodes), {});

    Diagnostics diags;
    SingleStartNodeRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors()[0].code == "E001");
}

// ====================== E002: SingleExitNode ======================

TEST_CASE("E002: valid graph has one EXIT node", "[validator][E002]") {
    auto graph = fixtures::make_simple_graph();
    Diagnostics diags;
    SingleExitNodeRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("E002: no EXIT node", "[validator][E002]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("s", NodeType::START));
    nodes.push_back(make_node("a", NodeType::CODERGEN));
    auto graph = make_graph(std::move(nodes), {});

    Diagnostics diags;
    SingleExitNodeRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors()[0].code == "E002");
}

// ====================== E003: StartNoIncoming ======================

TEST_CASE("E003: start with no incoming - passes", "[validator][E003]") {
    auto graph = fixtures::make_simple_graph();
    Diagnostics diags;
    StartNoIncomingRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("E003: start with incoming edge", "[validator][E003]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("start", NodeType::START));
    nodes.push_back(make_node("end", NodeType::EXIT));
    std::vector<Edge> edges;
    edges.push_back(make_edge("start", "end"));
    edges.push_back(make_edge("end", "start"));
    auto graph = make_graph(std::move(nodes), std::move(edges));

    Diagnostics diags;
    StartNoIncomingRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors()[0].code == "E003");
}

// ====================== E004: ExitNoOutgoing ======================

TEST_CASE("E004: exit with no outgoing - passes", "[validator][E004]") {
    auto graph = fixtures::make_simple_graph();
    Diagnostics diags;
    ExitNoOutgoingRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("E004: exit with outgoing edge", "[validator][E004]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("start", NodeType::START));
    nodes.push_back(make_node("end", NodeType::EXIT));
    std::vector<Edge> edges;
    edges.push_back(make_edge("start", "end"));
    edges.push_back(make_edge("end", "start"));
    auto graph = make_graph(std::move(nodes), std::move(edges));

    Diagnostics diags;
    ExitNoOutgoingRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors()[0].code == "E004");
}

// ====================== E005: AllNodesReachable ======================

TEST_CASE("E005: all nodes reachable - passes", "[validator][E005]") {
    auto graph = fixtures::make_simple_graph();
    Diagnostics diags;
    AllNodesReachableRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("E005: unreachable node", "[validator][E005]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("start", NodeType::START));
    nodes.push_back(make_node("end", NodeType::EXIT));
    nodes.push_back(make_node("orphan", NodeType::CODERGEN));
    std::vector<Edge> edges;
    edges.push_back(make_edge("start", "end"));
    auto graph = make_graph(std::move(nodes), std::move(edges));

    Diagnostics diags;
    AllNodesReachableRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors().size() == 1);
    REQUIRE(diags.errors()[0].code == "E005");
    REQUIRE(diags.errors()[0].node_id == "orphan");
}

// ====================== E006: ValidConditions ======================

TEST_CASE("E006: valid conditions - passes", "[validator][E006]") {
    auto graph = fixtures::make_conditional_graph();
    Diagnostics diags;
    ValidConditionsRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("E006: invalid condition", "[validator][E006]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("start", NodeType::START));
    nodes.push_back(make_node("end", NodeType::EXIT));
    std::vector<Edge> edges;
    edges.push_back(make_edge("start", "end", "===invalid==="));
    auto graph = make_graph(std::move(nodes), std::move(edges));

    Diagnostics diags;
    ValidConditionsRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors()[0].code == "E006");
}

// ====================== E007: ParallelFanInPairing ======================

TEST_CASE("E007: parallel with fan-in - passes", "[validator][E007]") {
    auto graph = fixtures::make_parallel_graph();
    Diagnostics diags;
    ParallelFanInPairingRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("E007: parallel without fan-in", "[validator][E007]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("start", NodeType::START));
    nodes.push_back(make_node("par", NodeType::PARALLEL));
    nodes.push_back(make_node("a", NodeType::CODERGEN));
    nodes.push_back(make_node("b", NodeType::CODERGEN));
    nodes.push_back(make_node("end", NodeType::EXIT));
    std::vector<Edge> edges;
    edges.push_back(make_edge("start", "par"));
    edges.push_back(make_edge("par", "a"));
    edges.push_back(make_edge("par", "b"));
    edges.push_back(make_edge("a", "end"));
    edges.push_back(make_edge("b", "end"));
    auto graph = make_graph(std::move(nodes), std::move(edges));

    Diagnostics diags;
    ParallelFanInPairingRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_errors());
    REQUIRE(diags.errors()[0].code == "E007");
}

// ====================== W001: NoSelfLoops ======================

TEST_CASE("W001: no self loops - passes", "[validator][W001]") {
    auto graph = fixtures::make_simple_graph();
    Diagnostics diags;
    NoSelfLoopsRule rule;
    rule.check(graph, diags);
    REQUIRE_FALSE(diags.has_warnings());
}

TEST_CASE("W001: self loop detected", "[validator][W001]") {
    std::vector<Node> nodes;
    nodes.push_back(make_node("start", NodeType::START));
    nodes.push_back(make_node("loop", NodeType::CODERGEN));
    nodes.push_back(make_node("end", NodeType::EXIT));
    std::vector<Edge> edges;
    edges.push_back(make_edge("start", "loop"));
    edges.push_back(make_edge("loop", "loop"));
    edges.push_back(make_edge("loop", "end"));
    auto graph = make_graph(std::move(nodes), std::move(edges));

    Diagnostics diags;
    NoSelfLoopsRule rule;
    rule.check(graph, diags);
    REQUIRE(diags.has_warnings());
    REQUIRE(diags.all()[0].code == "W001");
    REQUIRE(diags.all()[0].severity == DiagnosticSeverity::Warning);
}

// ====================== GraphValidator ======================

TEST_CASE("GraphValidator: create_default validates valid graph", "[validator]") {
    auto validator = GraphValidator::create_default();
    auto graph = fixtures::make_simple_graph();
    auto diags = validator.validate(graph);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("GraphValidator: create_default validates parallel graph", "[validator]") {
    auto validator = GraphValidator::create_default();
    auto graph = fixtures::make_parallel_graph();
    auto diags = validator.validate(graph);
    REQUIRE_FALSE(diags.has_errors());
}

TEST_CASE("GraphValidator: create_default catches multiple issues", "[validator]") {
    // Empty graph - no START, no EXIT
    auto graph = make_graph({}, {});
    auto validator = GraphValidator::create_default();
    auto diags = validator.validate(graph);
    REQUIRE(diags.has_errors());
    // Should have at least E001 and E002
    auto errors = diags.errors();
    bool has_e001 = false, has_e002 = false;
    for (const auto& e : errors) {
        if (e.code == "E001") has_e001 = true;
        if (e.code == "E002") has_e002 = true;
    }
    REQUIRE(has_e001);
    REQUIRE(has_e002);
}

TEST_CASE("GraphValidator: rejects params=name=value shorthand", "[validator]") {
    Node s = make_node("start", NodeType::START);
    Node e = make_node("end", NodeType::EXIT);
    Edge se = make_edge("start", "end");
    AttributeMap attrs;
    attrs.set("params", "repo_dir=/tmp/repo, run_mode:text:required");
    Graph graph = Graph::make("g", {s, e}, {se}, attrs);

    auto validator = GraphValidator::create_default();
    auto diags = validator.validate(graph);
    REQUIRE(diags.has_errors());
    bool found = false;
    for (const auto& d : diags.all()) {
        if (d.code == "E008") {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("GraphValidator: graph-level params= shorthand (name=value) reaches E008 before variable lint", "[validator][E008]") {
    // Regression for SPRINT-014 Phase 4: a graph using the
    // params='name=value, ...' shorthand (instead of the
    // params='name:type:default' template-declaration form) reached
    // unresolved-variable linting at run time instead of failing
    // validation with E008 at authoring time.
    auto graph = parse_graph_or_fail(R"dot(
digraph G {
  graph[params='repo_dir=/proj/repo, spec_path=/tmp/spec.md, roadmap_path=/tmp/roadmap.md'];
  start [shape=Mdiamond, prompt="use $var.repo_dir"];
  spec [shape=box, prompt="read $var.spec_path"];
  road [shape=box, prompt="read $var.roadmap_path"];
  end [shape=Msquare];
  start -> spec -> road -> end;
}
)dot");

    auto validator = GraphValidator::create_default();
    auto diags = validator.validate(graph);
    REQUIRE(diags.has_errors());
    bool found = false;
    for (const auto& d : diags.all()) {
        if (d.code == "E008") {
            found = true;
        }
    }
    REQUIRE(found);
}

// ====================== Diagnostics ======================

TEST_CASE("Diagnostics: errors vs warnings classification", "[validator][diagnostics]") {
    Diagnostics diags;
    Diagnostic d1;
    d1.severity = DiagnosticSeverity::Error;
    d1.code = "E001";
    d1.message = "test error";
    diags.add(d1);

    Diagnostic d2;
    d2.severity = DiagnosticSeverity::Warning;
    d2.code = "W001";
    d2.message = "test warning";
    diags.add(d2);

    Diagnostic d3;
    d3.severity = DiagnosticSeverity::Info;
    d3.code = "I001";
    d3.message = "test info";
    diags.add(d3);

    REQUIRE(diags.has_errors());
    REQUIRE(diags.has_warnings());
    REQUIRE(diags.all().size() == 3);
    REQUIRE(diags.errors().size() == 1);
}

TEST_CASE("Diagnostics: print outputs formatted text", "[validator][diagnostics]") {
    Diagnostics diags;
    Diagnostic d;
    d.severity = DiagnosticSeverity::Error;
    d.code = "E001";
    d.message = "test error";
    d.node_id = "start";
    diags.add(d);

    std::ostringstream out;
    diags.print(out, false);
    std::string output = out.str();
    REQUIRE(output.find("ERROR") != std::string::npos);
    REQUIRE(output.find("E001") != std::string::npos);
    REQUIRE(output.find("start") != std::string::npos);
    REQUIRE(output.find("test error") != std::string::npos);
}

TEST_CASE("Diagnostics: no errors in empty diagnostics", "[validator][diagnostics]") {
    Diagnostics diags;
    REQUIRE_FALSE(diags.has_errors());
    REQUIRE_FALSE(diags.has_warnings());
    REQUIRE(diags.all().empty());
    REQUIRE(diags.errors().empty());
}
