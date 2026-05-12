#include <catch2/catch.hpp>
#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/validation/graph_validator.h"
#include "needle/validation/dot_linter.h"

using namespace needle;

TEST_CASE("Regression stress fixture validates and lints", "[integration][regression_stress]") {
    DotParser p(
        "digraph regression_stress { graph [params=\"repo_dir:string:required\"]; start [shape=Mdiamond]; end [shape=Msquare]; a [prompt=\"$var.missing\"]; start->a->end; }");
    auto ast = p.parse();
    REQUIRE(ast.ok());
    GraphBuilder b;
    auto gr = b.build(ast.value());
    REQUIRE(gr.ok());
    Graph g = gr.value();

    auto diags = GraphValidator::create_default().validate(g);
    REQUIRE_FALSE(diags.has_errors());

    DotLinter l;
    auto ws = l.lint(g, {});
    REQUIRE_FALSE(ws.empty());
}
