#include <catch2/catch.hpp>
#include "needle/parser/graph_builder.h"
#include "needle/parser/dot_parser.h"
#include "helpers/graph_fixtures.h"

using namespace needle;

static Result<Graph> parse_and_build(const std::string& dot) {
    DotParser parser(dot);
    auto ast_result = parser.parse();
    if (!ast_result.ok()) {
        return Result<Graph>::failure(ast_result.error());
    }
    GraphBuilder builder;
    return builder.build(ast_result.value());
}

TEST_CASE("GraphBuilder: shape Mdiamond -> START", "[builder]") {
    auto result = parse_and_build("digraph G { s [shape=Mdiamond] }");
    REQUIRE(result.ok());
    const Node* n = result.value().find_node("s");
    REQUIRE(n != nullptr);
    REQUIRE(n->type == NodeType::START);
}

TEST_CASE("GraphBuilder: shape Msquare -> EXIT", "[builder]") {
    auto result = parse_and_build("digraph G { e [shape=Msquare] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("e")->type == NodeType::EXIT);
}

TEST_CASE("GraphBuilder: shape box -> CODERGEN", "[builder]") {
    auto result = parse_and_build("digraph G { n [shape=box] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::CODERGEN);
}

TEST_CASE("GraphBuilder: shape diamond -> CONDITIONAL", "[builder]") {
    auto result = parse_and_build("digraph G { n [shape=diamond] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::CONDITIONAL);
}

TEST_CASE("GraphBuilder: shape component -> PARALLEL", "[builder]") {
    auto result = parse_and_build("digraph G { n [shape=component] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::PARALLEL);
}

TEST_CASE("GraphBuilder: shape trapezium -> FAN_IN", "[builder]") {
    auto result = parse_and_build("digraph G { n [shape=trapezium] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::FAN_IN);
}

TEST_CASE("GraphBuilder: shape hexagon -> WAIT_HUMAN", "[builder]") {
    auto result = parse_and_build("digraph G { n [shape=hexagon] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::WAIT_HUMAN);
}

TEST_CASE("GraphBuilder: shape parallelogram -> TOOL", "[builder]") {
    auto result = parse_and_build("digraph G { n [shape=parallelogram] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::TOOL);
}

TEST_CASE("GraphBuilder: shape house -> MANAGER_LOOP", "[builder]") {
    auto result = parse_and_build("digraph G { n [shape=house] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::MANAGER_LOOP);
}

TEST_CASE("GraphBuilder: missing shape defaults to CODERGEN", "[builder]") {
    auto result = parse_and_build("digraph G { n [label=\"test\"] }");
    REQUIRE(result.ok());
    REQUIRE(result.value().find_node("n")->type == NodeType::CODERGEN);
}

TEST_CASE("GraphBuilder: edge chain expansion", "[builder]") {
    auto result = parse_and_build("digraph G { A -> B -> C [label=\"x\"] }");
    REQUIRE(result.ok());
    const auto& edges = result.value().edges();
    REQUIRE(edges.size() == 2);
    REQUIRE(edges[0].from == "A");
    REQUIRE(edges[0].to == "B");
    REQUIRE(edges[0].label() == "x");
    REQUIRE(edges[1].from == "B");
    REQUIRE(edges[1].to == "C");
    REQUIRE(edges[1].label() == "x");
}

TEST_CASE("GraphBuilder: node ID sanitization", "[builder]") {
    auto result = parse_and_build("digraph G { \"node with spaces\" [shape=box] }");
    REQUIRE(result.ok());
    // Spaces should be removed
    const Node* n = result.value().find_node("nodewithspaces");
    REQUIRE(n != nullptr);
}

TEST_CASE("GraphBuilder: default attribute inheritance", "[builder]") {
    auto result = parse_and_build(
        "digraph G {\n"
        "  subgraph cluster_0 {\n"
        "    node [shape=box]\n"
        "    A\n"
        "    B\n"
        "  }\n"
        "}"
    );
    REQUIRE(result.ok());
    const Node* a = result.value().find_node("A");
    const Node* b = result.value().find_node("B");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->type == NodeType::CODERGEN);
    REQUIRE(b->type == NodeType::CODERGEN);
}

TEST_CASE("GraphBuilder: graph name", "[builder]") {
    auto result = parse_and_build("digraph my_pipeline { }");
    REQUIRE(result.ok());
    REQUIRE(result.value().name() == "my_pipeline");
}

TEST_CASE("Graph fixtures: simple graph", "[fixtures]") {
    Graph g = fixtures::make_simple_graph();
    REQUIRE(g.nodes().size() == 3);
    REQUIRE(g.edges().size() == 2);
    REQUIRE(g.start_node() != nullptr);
    REQUIRE(g.exit_node() != nullptr);
}

TEST_CASE("Graph fixtures: parallel graph", "[fixtures]") {
    Graph g = fixtures::make_parallel_graph();
    REQUIRE(g.nodes().size() == 6);
    REQUIRE(g.edges().size() == 6);
}
