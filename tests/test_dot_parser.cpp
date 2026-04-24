#include <catch2/catch.hpp>
#include "needle/parser/dot_parser.h"

using namespace needle;

TEST_CASE("Parser: simple digraph", "[parser]") {
    DotParser parser("digraph G { }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().id == "G");
    REQUIRE_FALSE(result.value().strict);
}

TEST_CASE("Parser: strict digraph", "[parser]") {
    DotParser parser("strict digraph G { }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().strict);
}

TEST_CASE("Parser: node with attributes", "[parser]") {
    DotParser parser("digraph G { A [shape=box, label=\"Hello\"] }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().nodes.size() == 1);
    REQUIRE(result.value().nodes[0].id == "A");
    REQUIRE(result.value().nodes[0].attrs.size() == 2);
    REQUIRE(result.value().nodes[0].attrs[0].key == "shape");
    REQUIRE(result.value().nodes[0].attrs[0].value == "box");
    REQUIRE(result.value().nodes[0].attrs[1].key == "label");
    REQUIRE(result.value().nodes[0].attrs[1].value == "Hello");
}

TEST_CASE("Parser: edge statement", "[parser]") {
    DotParser parser("digraph G { A -> B }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().edges.size() == 1);
    REQUIRE(result.value().edges[0].node_chain.size() == 2);
    REQUIRE(result.value().edges[0].node_chain[0] == "A");
    REQUIRE(result.value().edges[0].node_chain[1] == "B");
}

TEST_CASE("Parser: edge with attributes", "[parser]") {
    DotParser parser("digraph G { A -> B [label=\"test\"] }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().edges.size() == 1);
    REQUIRE(result.value().edges[0].attrs.size() == 1);
    REQUIRE(result.value().edges[0].attrs[0].key == "label");
    REQUIRE(result.value().edges[0].attrs[0].value == "test");
}

TEST_CASE("Parser: edge chain", "[parser]") {
    DotParser parser("digraph G { A -> B -> C }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().edges.size() == 1);
    REQUIRE(result.value().edges[0].node_chain.size() == 3);
    REQUIRE(result.value().edges[0].node_chain[0] == "A");
    REQUIRE(result.value().edges[0].node_chain[1] == "B");
    REQUIRE(result.value().edges[0].node_chain[2] == "C");
}

TEST_CASE("Parser: edge chain with attributes", "[parser]") {
    DotParser parser("digraph G { A -> B -> C [label=\"x\"] }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    const auto& edge = result.value().edges[0];
    REQUIRE(edge.node_chain.size() == 3);
    REQUIRE(edge.attrs.size() == 1);
    REQUIRE(edge.attrs[0].value == "x");
}

TEST_CASE("Parser: default node attributes", "[parser]") {
    DotParser parser("digraph G { node [shape=box]; A; B; }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().defaults.size() == 1);
    REQUIRE(result.value().defaults[0].target == "node");
    REQUIRE(result.value().defaults[0].attrs[0].key == "shape");
    REQUIRE(result.value().defaults[0].attrs[0].value == "box");
    REQUIRE(result.value().nodes.size() == 2);
}

TEST_CASE("Parser: subgraph", "[parser]") {
    DotParser parser("digraph G { subgraph cluster_0 { A; B; } }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().subgraphs.size() == 1);
    REQUIRE(result.value().subgraphs[0].id == "cluster_0");
    REQUIRE(result.value().subgraphs[0].nodes.size() == 2);
}

TEST_CASE("Parser: empty graph", "[parser]") {
    DotParser parser("digraph { }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().id.empty());
    REQUIRE(result.value().nodes.empty());
    REQUIRE(result.value().edges.empty());
}

TEST_CASE("Parser: optional semicolons", "[parser]") {
    DotParser parser("digraph G { A; B; A -> B; }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().nodes.size() == 2);
    REQUIRE(result.value().edges.size() == 1);
}

TEST_CASE("Parser: semicolons not required", "[parser]") {
    DotParser parser("digraph G { A B A -> B }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().nodes.size() == 2);
    REQUIRE(result.value().edges.size() == 1);
}

TEST_CASE("Parser: error reporting with line numbers", "[parser]") {
    DotParser parser("digraph G {\n  A ->\n}");
    auto result = parser.parse();
    REQUIRE_FALSE(result.ok());
    // Should mention line info in error
    REQUIRE(result.error().find("line") != std::string::npos);
}

TEST_CASE("Parser: case-insensitive keywords", "[parser]") {
    DotParser parser("DIGRAPH G { }");
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().id == "G");
}

TEST_CASE("Parser: graph keyword", "[parser]") {
    DotParser parser("graph G { }");
    auto result = parser.parse();
    REQUIRE(result.ok());
}

TEST_CASE("Parser: multiple nodes and edges", "[parser]") {
    DotParser parser(
        "digraph pipeline {\n"
        "  start [shape=Mdiamond]\n"
        "  work [shape=box, label=\"Do Work\"]\n"
        "  end_node [shape=Msquare]\n"
        "  start -> work -> end_node\n"
        "}"
    );
    auto result = parser.parse();
    REQUIRE(result.ok());
    REQUIRE(result.value().id == "pipeline");
    REQUIRE(result.value().nodes.size() == 3);
    REQUIRE(result.value().edges.size() == 1);
    REQUIRE(result.value().edges[0].node_chain.size() == 3);
}
