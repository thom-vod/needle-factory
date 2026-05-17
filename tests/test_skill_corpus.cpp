// SPRINT-013 Phase 5: skill-corpus integration tests.
//
// Each TEST_CASE in this file mirrors a documented pattern from
// `docs/dot-authoring-rules.md` or `skills/needle-pipeline/SKILL.md`.
// If the engine ever stops honouring one of those patterns, the
// corresponding test fails.
//
// The harness: parse → build → apply stylesheets → apply variable
// expansion → inspect node attributes against expected values. No
// real backend is invoked.

#include <catch2/catch.hpp>

#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/engine/transform.h"
#include "needle/engine/variable_expansion_transform.h"
#include "needle/parser/stylesheet_parser.h"
#include "needle/model/context.h"
#include "needle/model/graph.h"

using namespace needle;

namespace {

// Build a graph from DOT source and run the full graph-load transform
// chain against `ctx`: parse → graph build → inline stylesheet → variable
// expansion. Returns the resulting graph for attribute assertions.
Graph load_and_transform(const std::string& dot_source, const Context& ctx) {
    DotParser parser(dot_source);
    auto ast = parser.parse();
    REQUIRE(ast.ok());

    GraphBuilder builder;
    auto built = builder.build(ast.value());
    REQUIRE(built.ok());
    Graph graph = std::move(built.value());

    // Apply inline `graph[model_stylesheet="..."]` if present.
    std::string ss_src = graph.graph_attrs().get("model_stylesheet");
    if (!ss_src.empty()) {
        auto ss_result = StylesheetParser::parse(ss_src);
        REQUIRE(ss_result.ok());
        auto transform = make_stylesheet_transform(ss_result.value());
        Context tmp;
        transform->apply(graph, tmp);
    }

    auto ve = make_variable_expansion_transform();
    auto result = ve->apply(graph, ctx);
    REQUIRE(result.ok());

    return graph;
}

} // anonymous namespace

// Documented in dot-authoring-rules.md §"Variable expansion reference"
// and §"Stylesheet examples": `$context.config.defaults.*` references in
// `llm_model` / `agent` / `llm_provider` are early-bound and must
// resolve to the run's configured values. The user-reported bug that
// drove SPRINT-013 was exactly this case being silently ignored.
TEST_CASE("Skill corpus: stylesheet + $context.config.defaults.* resolves on llm_model + agent",
          "[skill_corpus][stylesheet][variable]") {
    const std::string dot = R"(
digraph g {
    graph [model_stylesheet="
        * { agent = \"$context.config.defaults.coding_agent\"; llm_model = \"$context.config.defaults.coding_model\" }
        .planning { agent = \"$context.config.defaults.planning_agent\"; llm_model = \"$context.config.defaults.planning_model\" }
    "]
    start [shape=Mdiamond]
    plan [class=planning, prompt="plan"]
    impl [prompt="impl"]
    exit [shape=Msquare]
    start -> plan -> impl -> exit
}
)";

    Context ctx;
    ctx.set("config.defaults.coding_agent", "codex");
    ctx.set("config.defaults.coding_model", "gpt-5.4");
    ctx.set("config.defaults.planning_agent", "claude");
    ctx.set("config.defaults.planning_model", "claude-opus-4-7");

    Graph g = load_and_transform(dot, ctx);

    auto* plan = g.find_node("plan");
    REQUIRE(plan);
    REQUIRE(plan->attrs.get("agent") == "claude");
    REQUIRE(plan->attrs.get("llm_model") == "claude-opus-4-7");

    auto* impl = g.find_node("impl");
    REQUIRE(impl);
    REQUIRE(impl->attrs.get("agent") == "codex");
    REQUIRE(impl->attrs.get("llm_model") == "gpt-5.4");
}

// Documented in dot-authoring-rules.md: `$var.<name>` expands from
// run-supplied template parameters (CLI `--var name=value` or
// dashboard's params= form).
TEST_CASE("Skill corpus: $var.X resolves from CLI/dashboard-supplied parameters",
          "[skill_corpus][variable]") {
    const std::string dot = R"(
digraph g {
    start [shape=Mdiamond]
    plan [prompt="Implement $var.feature in $var.repo_dir."]
    exit [shape=Msquare]
    start -> plan -> exit
}
)";

    Context ctx;
    ctx.set("var.feature", "auth middleware");
    ctx.set("var.repo_dir", "/proj/repo");

    Graph g = load_and_transform(dot, ctx);

    auto* plan = g.find_node("plan");
    REQUIRE(plan);
    REQUIRE(plan->attrs.get("prompt") ==
            "Implement auth middleware in /proj/repo.");
}

// Regression: variable references followed by sentence-terminal
// punctuation (`.`, `,`, `)`) should resolve cleanly without dragging
// the punctuation into the identifier (SPRINT-009 fix).
TEST_CASE("Skill corpus: $var.X. with trailing period resolves cleanly",
          "[skill_corpus][variable]") {
    const std::string dot = R"(
digraph g {
    start [shape=Mdiamond]
    orient [prompt="Read $var.spec_path. Read $var.roadmap_path, then plan."]
    exit [shape=Msquare]
    start -> orient -> exit
}
)";

    Context ctx;
    ctx.set("var.spec_path", "/tmp/spec.md");
    ctx.set("var.roadmap_path", "/tmp/roadmap.md");

    Graph g = load_and_transform(dot, ctx);

    auto* orient = g.find_node("orient");
    REQUIRE(orient);
    REQUIRE(orient->attrs.get("prompt") ==
            "Read /tmp/spec.md. Read /tmp/roadmap.md, then plan.");
}

// Documented: `{{logs_dir}}` is a placeholder substituted from
// `needle.logs_dir`. Skill rules require it for log paths.
TEST_CASE("Skill corpus: {{logs_dir}} placeholder substitutes from needle.logs_dir",
          "[skill_corpus][placeholder]") {
    const std::string dot = R"(
digraph g {
    start [shape=Mdiamond]
    impl [prompt="Read prior artifacts at {{logs_dir}}/impl/*.md"]
    exit [shape=Msquare]
    start -> impl -> exit
}
)";

    Context ctx;
    ctx.set("needle.logs_dir", "/proj/.needle/foo/logs");

    Graph g = load_and_transform(dot, ctx);

    auto* impl = g.find_node("impl");
    REQUIRE(impl);
    REQUIRE(impl->attrs.get("prompt") ==
            "Read prior artifacts at /proj/.needle/foo/logs/impl/*.md");
}

// Shell-style $VAR / regex backrefs / Svelte runes must survive
// expansion untouched — they look variable-shaped but aren't needle
// template references.
TEST_CASE("Skill corpus: tool command preserves shell $VAR and regex $",
          "[skill_corpus][variable][shell]") {
    const std::string dot = R"(
digraph g {
    start [shape=Mdiamond]
    build [shape=box, type=tool, command="echo $HOME && grep '^foo$' /etc/hostname && echo $1"]
    exit [shape=Msquare]
    start -> build -> exit
}
)";

    Context ctx;

    Graph g = load_and_transform(dot, ctx);

    auto* build = g.find_node("build");
    REQUIRE(build);
    REQUIRE(build->attrs.get("command") ==
            "echo $HOME && grep '^foo$' /etc/hostname && echo $1");
}
