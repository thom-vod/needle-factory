#include <catch2/catch.hpp>
#include "needle/engine/transform.h"
#include "needle/parser/stylesheet_parser.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"

using namespace needle;

namespace {

Graph make_test_graph() {
    std::vector<Node> nodes;

    Node n1;
    n1.id = "start";
    n1.type = NodeType::START;
    nodes.push_back(std::move(n1));

    Node n2;
    n2.id = "implement";
    n2.type = NodeType::CODERGEN;
    n2.attrs.set("class", "code");
    nodes.push_back(std::move(n2));

    Node n3;
    n3.id = "review";
    n3.type = NodeType::CODERGEN;
    n3.attrs.set("class", "code");
    nodes.push_back(std::move(n3));

    Node n4;
    n4.id = "exit";
    n4.type = NodeType::EXIT;
    nodes.push_back(std::move(n4));

    std::vector<Edge> edges;
    return Graph::make("test", std::move(nodes), std::move(edges));
}

} // anonymous namespace

TEST_CASE("StylesheetTransform: universal rule applies to all nodes", "[transform]") {
    auto parse_result = StylesheetParser::parse("* { llm_model: claude-sonnet; }");
    REQUIRE(parse_result.ok());

    auto transform = make_stylesheet_transform(parse_result.value());
    Graph graph = make_test_graph();
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    for (const auto& node : graph.nodes()) {
        REQUIRE(node.attrs.get("llm_model") == "claude-sonnet");
    }
}

TEST_CASE("StylesheetTransform: class rule applies to matching nodes only", "[transform]") {
    auto parse_result = StylesheetParser::parse(".code { llm_model: claude-opus; }");
    REQUIRE(parse_result.ok());

    auto transform = make_stylesheet_transform(parse_result.value());
    Graph graph = make_test_graph();
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    // Only nodes with class=code should get the property
    REQUIRE(graph.find_node("implement")->attrs.get("llm_model") == "claude-opus");
    REQUIRE(graph.find_node("review")->attrs.get("llm_model") == "claude-opus");
    REQUIRE(graph.find_node("start")->attrs.get("llm_model") == "");
    REQUIRE(graph.find_node("exit")->attrs.get("llm_model") == "");
}

TEST_CASE("StylesheetTransform: ID rule overrides class rule", "[transform]") {
    std::string css =
        ".code { llm_model: claude-sonnet; }\n"
        "#implement { llm_model: claude-opus; }";
    auto parse_result = StylesheetParser::parse(css);
    REQUIRE(parse_result.ok());

    auto transform = make_stylesheet_transform(parse_result.value());
    Graph graph = make_test_graph();
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    // implement gets ID rule (opus), review gets class rule (sonnet)
    REQUIRE(graph.find_node("implement")->attrs.get("llm_model") == "claude-opus");
    REQUIRE(graph.find_node("review")->attrs.get("llm_model") == "claude-sonnet");
}

TEST_CASE("StylesheetTransform: node attrs override stylesheet", "[transform]") {
    auto parse_result = StylesheetParser::parse("* { llm_model: claude-sonnet; }");
    REQUIRE(parse_result.ok());

    auto transform = make_stylesheet_transform(parse_result.value());
    Graph graph = make_test_graph();

    // Set explicit attr on one node
    graph.mutable_node("implement")->attrs.set("llm_model", "gpt-4");

    Context ctx;
    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    // Node-level attr should be preserved
    REQUIRE(graph.find_node("implement")->attrs.get("llm_model") == "gpt-4");
    // Other nodes get stylesheet value
    REQUIRE(graph.find_node("review")->attrs.get("llm_model") == "claude-sonnet");
}

TEST_CASE("StylesheetTransform: specificity ordering universal < class < id", "[transform]") {
    std::string css =
        "#implement { priority: id; }\n"
        "* { priority: universal; }\n"
        ".code { priority: class; }";
    auto parse_result = StylesheetParser::parse(css);
    REQUIRE(parse_result.ok());

    auto transform = make_stylesheet_transform(parse_result.value());
    Graph graph = make_test_graph();
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    // implement has class=code and id=implement
    REQUIRE(graph.find_node("implement")->attrs.get("priority") == "id");
    // review has class=code but no ID rule
    REQUIRE(graph.find_node("review")->attrs.get("priority") == "class");
    // start has no class and no ID rule
    REQUIRE(graph.find_node("start")->attrs.get("priority") == "universal");
}

TEST_CASE("StylesheetTransform: empty stylesheet is a no-op", "[transform]") {
    Stylesheet empty;
    auto transform = make_stylesheet_transform(empty);
    Graph graph = make_test_graph();
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
}

TEST_CASE("StylesheetTransform: multiple properties per rule", "[transform]") {
    std::string css = "* { llm_model: claude-sonnet; temperature: 0.7; max_tokens: 4096; }";
    auto parse_result = StylesheetParser::parse(css);
    REQUIRE(parse_result.ok());

    auto transform = make_stylesheet_transform(parse_result.value());
    Graph graph = make_test_graph();
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("start")->attrs.get("llm_model") == "claude-sonnet");
    REQUIRE(graph.find_node("start")->attrs.get("temperature") == "0.7");
    REQUIRE(graph.find_node("start")->attrs.get("max_tokens") == "4096");
}
