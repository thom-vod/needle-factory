#include <catch2/catch.hpp>

#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/validation/dot_linter.h"

using namespace needle;

static Graph parse_graph(const std::string& dot) {
    DotParser p(dot);
    auto ast = p.parse();
    REQUIRE(ast.ok());
    GraphBuilder b;
    auto g = b.build(ast.value());
    REQUIRE(g.ok());
    return g.value();
}

TEST_CASE("DotLinter emits W001 for undeclared var", "[dot_lint]") {
    Graph g = parse_graph(
        "digraph t { start [shape=Mdiamond]; end [shape=Msquare]; a [prompt=\"x $var.missing\"]; start->a->end; }");
    DotLinter l;
    auto ws = l.lint(g, {});
    bool found = false;
    for (const auto& w : ws) if (w.code == "W001") found = true;
    REQUIRE(found);
}

TEST_CASE("DotLinter suppresses warnings via lint_suppress", "[dot_lint]") {
    Graph g = parse_graph(
        "digraph t { graph [lint_suppress=\"W001\"]; start [shape=Mdiamond]; end [shape=Msquare]; a [prompt=\"x $var.missing\"]; start->a->end; }");
    DotLinter l;
    auto ws = l.lint(g, {});
    bool found = false;
    for (const auto& w : ws) if (w.code == "W001") found = true;
    REQUIRE_FALSE(found);
}
