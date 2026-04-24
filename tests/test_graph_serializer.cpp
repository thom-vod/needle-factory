#include <catch2/catch.hpp>
#include "needle/util/graph_serializer.h"
#include "helpers/graph_fixtures.h"

using namespace needle;

TEST_CASE("html_escape replaces special characters", "[graph_serializer]") {
    CHECK(html_escape("hello") == "hello");
    CHECK(html_escape("<b>bold</b>") == "&lt;b&gt;bold&lt;/b&gt;");
    CHECK(html_escape("a & b") == "a &amp; b");
    CHECK(html_escape("say \"hi\"") == "say &quot;hi&quot;");
    CHECK(html_escape("it's") == "it&#39;s");
    CHECK(html_escape("") == "");
    CHECK(html_escape("<>&\"'") == "&lt;&gt;&amp;&quot;&#39;");
}

TEST_CASE("graph_to_dot produces valid DOT output", "[graph_serializer]") {
    Graph g = fixtures::make_simple_graph();
    std::string dot = graph_to_dot(g);

    SECTION("has digraph wrapper") {
        CHECK(dot.find("digraph \"simple\"") != std::string::npos);
        CHECK(dot.find("rankdir=TB") != std::string::npos);
    }

    SECTION("contains all nodes") {
        CHECK(dot.find("\"start\"") != std::string::npos);
        CHECK(dot.find("\"work\"") != std::string::npos);
        CHECK(dot.find("\"end\"") != std::string::npos);
    }

    SECTION("contains edges") {
        CHECK(dot.find("\"start\" -> \"work\"") != std::string::npos);
        CHECK(dot.find("\"work\" -> \"end\"") != std::string::npos);
    }

    SECTION("uses correct shapes") {
        CHECK(dot.find("shape=circle") != std::string::npos);       // START
        CHECK(dot.find("shape=doublecircle") != std::string::npos); // EXIT
        CHECK(dot.find("shape=box") != std::string::npos);          // CODERGEN
    }
}

TEST_CASE("graph_to_dot handles conditional graph with edge labels", "[graph_serializer]") {
    Graph g = fixtures::make_conditional_graph();
    std::string dot = graph_to_dot(g);

    CHECK(dot.find("shape=diamond") != std::string::npos);  // CONDITIONAL
    CHECK(dot.find("label=\"Yes\"") != std::string::npos);
    CHECK(dot.find("label=\"No\"") != std::string::npos);
}

TEST_CASE("graph_to_dot handles parallel graph", "[graph_serializer]") {
    Graph g = fixtures::make_parallel_graph();
    std::string dot = graph_to_dot(g);

    CHECK(dot.find("shape=doubleoctagon") != std::string::npos);  // PARALLEL / FAN_IN
    CHECK(dot.find("\"fork\"") != std::string::npos);
    CHECK(dot.find("\"join\"") != std::string::npos);
}

TEST_CASE("graph_to_dot handles empty graph name", "[graph_serializer]") {
    Graph g = Graph::make("", {}, {});
    std::string dot = graph_to_dot(g);

    CHECK(dot.find("digraph \"pipeline\"") != std::string::npos);
}

TEST_CASE("graph_to_dot escapes special characters in names", "[graph_serializer]") {
    std::vector<Node> nodes;
    {
        Node n; n.id = "node\"special"; n.type = NodeType::START;
        n.attrs.set("label", "A \"quoted\" label");
        nodes.push_back(std::move(n));
    }
    Graph g = Graph::make("test\\graph", std::move(nodes), {});
    std::string dot = graph_to_dot(g);

    CHECK(dot.find("test\\\\graph") != std::string::npos);
    CHECK(dot.find("node\\\"special") != std::string::npos);
}

TEST_CASE("dot_to_svg returns empty for empty input", "[graph_serializer]") {
    CHECK(dot_to_svg("").empty());
}
