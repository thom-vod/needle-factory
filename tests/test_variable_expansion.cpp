#include <catch2/catch.hpp>
#include "needle/engine/transform.h"
#include "needle/engine/variable_expansion_transform.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"

using namespace needle;

namespace {

Graph make_graph_with_prompt(const std::string& prompt,
                             const std::string& goal = "") {
    std::vector<Node> nodes;

    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", prompt);
    if (!goal.empty()) {
        n.attrs.set("goal", goal);
    }
    nodes.push_back(std::move(n));

    AttributeMap graph_attrs;
    if (!goal.empty()) {
        graph_attrs.set("goal", "graph-level-goal");
    }

    return Graph::make("test", std::move(nodes), std::vector<Edge>(), std::move(graph_attrs));
}

} // anonymous namespace

TEST_CASE("VariableExpansionTransform: expand ${context.key}", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    Graph graph = make_graph_with_prompt("Hello ${context.name}, your project is ${context.project}.");
    Context ctx;
    ctx.set("name", "Alice");
    ctx.set("project", "needle");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "Hello Alice, your project is needle.");
}

TEST_CASE("VariableExpansionTransform: missing context key stays unexpanded", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    Graph graph = make_graph_with_prompt("Value: ${context.missing}");
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") == "Value: ${context.missing}");
}

TEST_CASE("VariableExpansionTransform: $goal expands from node attr", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    Graph graph = make_graph_with_prompt("Achieve: $goal", "build the feature");
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") == "Achieve: build the feature");
}

TEST_CASE("VariableExpansionTransform: $goal falls back to graph attr", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "Goal is: $goal");
    nodes.push_back(std::move(n));

    AttributeMap graph_attrs;
    graph_attrs.set("goal", "ship it");

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>(), std::move(graph_attrs));
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") == "Goal is: ship it");
}

TEST_CASE("VariableExpansionTransform: no variables is a no-op", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    Graph graph = make_graph_with_prompt("Plain text without any variables");
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") == "Plain text without any variables");
}

TEST_CASE("VariableExpansionTransform: mixed variables", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "Goal: $goal, Name: ${context.name}");
    n.attrs.set("goal", "test");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("name", "Bob");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") == "Goal: test, Name: Bob");
}

TEST_CASE("VariableExpansionTransform: $goal at end of string", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "Achieve $goal");
    n.attrs.set("goal", "success");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") == "Achieve success");
}

TEST_CASE("VariableExpansionTransform: $goals is not expanded (partial match)", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "Multiple $goals");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    // $goals should NOT be expanded because "goals" != "goal"
    REQUIRE(graph.find_node("work")->attrs.get("prompt") == "Multiple $goals");
}

// ─── New expanded attributes ─────────────────────────────────────────

TEST_CASE("VariableExpansionTransform: provider attribute expanded", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "do stuff");
    n.attrs.set("provider", "$var.backend");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("var.backend", "anthropic");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
    REQUIRE(graph.find_node("work")->attrs.get("provider") == "anthropic");
}

TEST_CASE("VariableExpansionTransform: mode attribute expanded", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "do stuff");
    n.attrs.set("mode", "${context.run_mode}");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("run_mode", "autonomous");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
    REQUIRE(graph.find_node("work")->attrs.get("mode") == "autonomous");
}

TEST_CASE("VariableExpansionTransform: fetch_type attribute expanded", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "fetch");
    n.attrs.set("fetch_type", "$var.fetch_method");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("var.fetch_method", "deep");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
    REQUIRE(graph.find_node("work")->attrs.get("fetch_type") == "deep");
}

TEST_CASE("VariableExpansionTransform: headed attribute expanded", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "browse");
    n.attrs.set("headed", "$var.use_headed");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("var.use_headed", "true");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
    REQUIRE(graph.find_node("work")->attrs.get("headed") == "true");
}

TEST_CASE("VariableExpansionTransform: timeout attribute expanded", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "search");
    n.attrs.set("timeout", "$var.timeout_sec");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("var.timeout_sec", "30");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
    REQUIRE(graph.find_node("work")->attrs.get("timeout") == "30");
}

TEST_CASE("VariableExpansionTransform: all new attributes expanded in single node", "[transform][variable]") {
    auto transform = make_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "do stuff");
    n.attrs.set("provider", "$var.p");
    n.attrs.set("mode", "$var.m");
    n.attrs.set("fetch_type", "$var.ft");
    n.attrs.set("headed", "$var.h");
    n.attrs.set("timeout", "$var.t");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("var.p", "openai");
    ctx.set("var.m", "interactive");
    ctx.set("var.ft", "shallow");
    ctx.set("var.h", "false");
    ctx.set("var.t", "60");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    auto* node = graph.find_node("work");
    REQUIRE(node->attrs.get("provider") == "openai");
    REQUIRE(node->attrs.get("mode") == "interactive");
    REQUIRE(node->attrs.get("fetch_type") == "shallow");
    REQUIRE(node->attrs.get("headed") == "false");
    REQUIRE(node->attrs.get("timeout") == "60");
}

// ─── Unresolved variable tracking ─────────────────────────────────────

TEST_CASE("VariableExpansionTransform: unresolved_vars tracks missing $var references", "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "seed: $var.missing_key");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    auto unresolved = transform->unresolved_vars();
    REQUIRE(unresolved.size() == 1);
    REQUIRE(unresolved[0].first == "work");
    REQUIRE(unresolved[0].second == "var.missing_key");
}

TEST_CASE("VariableExpansionTransform: unresolved_vars tracks missing ${context.key}", "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "value: ${context.not_set}");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    auto unresolved = transform->unresolved_vars();
    // $context.* vars are late-bound (resolved at runtime), not reported as unresolved
    REQUIRE(unresolved.size() == 0);
}

TEST_CASE("VariableExpansionTransform: unresolved_vars empty when all resolved", "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt", "hello $var.name");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;
    ctx.set("var.name", "world");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    auto unresolved = transform->unresolved_vars();
    REQUIRE(unresolved.empty());
}

TEST_CASE("VariableExpansionTransform: multiple unresolved vars across nodes", "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    {
        Node n;
        n.id = "step1";
        n.type = NodeType::CODERGEN;
        n.attrs.set("prompt", "$var.aaa");
        nodes.push_back(std::move(n));
    }
    {
        Node n;
        n.id = "step2";
        n.type = NodeType::CODERGEN;
        n.attrs.set("prompt", "$var.bbb and ${context.ccc}");
        nodes.push_back(std::move(n));
    }

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    // Only $var.* refs are unresolved; $context.* is late-bound and not reported
    auto unresolved = transform->unresolved_vars();
    REQUIRE(unresolved.size() == 2);
}

// ─── Bare $identifier pass-through (not a needle template ref) ───────────
//
// Prompts often contain literal $foo text — Svelte 5 runes ($state, $derived,
// $props, $effect), shell variables ($PATH), jQuery ($), regex backrefs ($1).
// These should pass through verbatim with no expansion attempt and no
// unresolved-variable warning. Only $<known-prefix>.* and $goal are treated
// as needle template references.

TEST_CASE("VariableExpansionTransform: Svelte runes pass through without warning",
          "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt",
        "let count = $state(0); let double = $derived(count * 2); "
        "let { name } = $props(); $effect(() => console.log(count));");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    // Prompt unchanged — every $rune is literal.
    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "let count = $state(0); let double = $derived(count * 2); "
            "let { name } = $props(); $effect(() => console.log(count));");
    // And no unresolved-variable noise.
    REQUIRE(transform->unresolved_vars().empty());
}

TEST_CASE("VariableExpansionTransform: shell-style and unknown bare $ident pass through",
          "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    n.attrs.set("prompt",
        "echo $PATH; case $1 in a) echo $HOME;; esac; $foo $unknown_thing");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "echo $PATH; case $1 in a) echo $HOME;; esac; $foo $unknown_thing");
    REQUIRE(transform->unresolved_vars().empty());
}

TEST_CASE("VariableExpansionTransform: unknown ${braced} also passes through",
          "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    // ${something} that isn't a known namespace is literal — no warning.
    n.attrs.set("prompt", "template literal: ${foo.bar} and ${random}");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "template literal: ${foo.bar} and ${random}");
    REQUIRE(transform->unresolved_vars().empty());
}

TEST_CASE("VariableExpansionTransform: known prefix unresolved still reported",
          "[transform][variable]") {
    auto transform = make_typed_variable_expansion_transform();

    std::vector<Node> nodes;
    Node n;
    n.id = "work";
    n.type = NodeType::CODERGEN;
    // Mix of literal Svelte rune (shouldn't warn) and real unresolved $var.*
    n.attrs.set("prompt", "let count = $state(0); input: $var.never_set");
    nodes.push_back(std::move(n));

    Graph graph = Graph::make("test", std::move(nodes), std::vector<Edge>());
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    auto unresolved = transform->unresolved_vars();
    REQUIRE(unresolved.size() == 1);
    REQUIRE(unresolved[0].second == "var.never_set");
}

TEST_CASE("VariableExpansionTransform: trailing punctuation does not join identifier",
          "[transform][variable]") {
    auto transform = make_variable_expansion_transform();
    Graph graph = make_graph_with_prompt(
        "Path: $var.repo_dir. comma $var.repo_dir, semi $var.repo_dir; colon $var.repo_dir: paren ($var.repo_dir)");
    Context ctx;
    ctx.set("var.repo_dir", "/tmp/repo");
    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "Path: /tmp/repo. comma /tmp/repo, semi /tmp/repo; colon /tmp/repo: paren (/tmp/repo)");
}

TEST_CASE("VariableExpansionTransform: $var.repo_dir prefers needle.branch.cwd",
          "[transform][variable][worktree]") {
    auto transform = make_variable_expansion_transform();
    Graph graph = make_graph_with_prompt("Repo: $var.repo_dir");
    Context ctx;
    ctx.set("var.repo_dir", "/parent/repo");
    ctx.set("needle.branch.cwd", "/parent/repo-wt-branch");
    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());
    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "Repo: /parent/repo-wt-branch");
}

TEST_CASE("VariableExpansionTransform: {{logs_dir}} expands from needle.logs_dir", "[transform][placeholder]") {
    auto transform = make_variable_expansion_transform();

    Graph graph = make_graph_with_prompt(
        "Read prior artifacts from {{logs_dir}}/orient/*-*.md then write {{logs_dir}}/orient/ORIENT-1.md.");
    Context ctx;
    ctx.set("needle.logs_dir", ".needle/android/logs");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "Read prior artifacts from .needle/android/logs/orient/*-*.md then write .needle/android/logs/orient/ORIENT-1.md.");
}

TEST_CASE("VariableExpansionTransform: {{unknown}} passes through unchanged", "[transform][placeholder]") {
    auto transform = make_variable_expansion_transform();

    Graph graph = make_graph_with_prompt("Template with {{custom_marker}} stays literal.");
    Context ctx;

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "Template with {{custom_marker}} stays literal.");
}

TEST_CASE("VariableExpansionTransform: \\{{logs_dir}} escape passes literal", "[transform][placeholder]") {
    auto transform = make_variable_expansion_transform();

    // Simulates the string after DOT lexing: a single backslash followed by {{logs_dir}}.
    // Meta-pipelines use this to teach an LLM to emit a literal placeholder
    // into the DOT it generates.
    Graph graph = make_graph_with_prompt("Emit \\{{logs_dir}}/build/*.md into the generated DOT.");
    Context ctx;
    ctx.set("needle.logs_dir", ".needle/meta/logs");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "Emit {{logs_dir}}/build/*.md into the generated DOT.");
}

TEST_CASE("VariableExpansionTransform: placeholder and $var interoperate", "[transform][placeholder]") {
    auto transform = make_variable_expansion_transform();

    Graph graph = make_graph_with_prompt("Project $var.name logs at {{logs_dir}}");
    Context ctx;
    ctx.set("var.name", "demo");
    ctx.set("needle.logs_dir", ".needle/demo/logs");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("work")->attrs.get("prompt") ==
            "Project demo logs at .needle/demo/logs");
}

TEST_CASE("VariableExpansionTransform: {{logs_dir}} survives DOT parsing end-to-end", "[transform][placeholder]") {
    // Confirms the lexer doesn't eat `{{` / `}}` inside a quoted attribute,
    // so DOT files authored with {{logs_dir}} placeholders reach the expander intact.
    const char* dot_source =
        "digraph t {\n"
        "    start [shape=Mdiamond, label=\"Start\"]\n"
        "    build [label=\"Build\", prompt=\"Read {{logs_dir}}/build/*.md\"]\n"
        "    exit  [shape=Msquare, label=\"Done\"]\n"
        "    start -> build -> exit\n"
        "}\n";

    DotParser parser(dot_source);
    auto ast = parser.parse();
    REQUIRE(ast.ok());
    GraphBuilder builder;
    auto built = builder.build(ast.value());
    REQUIRE(built.ok());
    Graph graph = std::move(built.value());

    auto transform = make_variable_expansion_transform();
    Context ctx;
    ctx.set("needle.logs_dir", ".needle/t/logs");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("build")->attrs.get("prompt") ==
            "Read .needle/t/logs/build/*.md");
}

TEST_CASE("VariableExpansionTransform: DOT-escaped \\\\{{logs_dir}} passes through lexer + expander", "[transform][placeholder]") {
    // A meta-pipeline writes `\\{{logs_dir}}` in the DOT source.
    // The lexer converts `\\` to `\`, yielding `\{{logs_dir}}` in the
    // string; the expander then recognizes the escape and emits `{{logs_dir}}`
    // verbatim so an LLM generating DOTs sees the literal placeholder.
    const char* dot_source =
        "digraph meta {\n"
        "    start [shape=Mdiamond, label=\"Start\"]\n"
        "    gen [label=\"Gen\", prompt=\"Emit \\\\{{logs_dir}}/build literally\"]\n"
        "    exit  [shape=Msquare, label=\"Done\"]\n"
        "    start -> gen -> exit\n"
        "}\n";

    DotParser parser(dot_source);
    auto ast = parser.parse();
    REQUIRE(ast.ok());
    GraphBuilder builder;
    auto built = builder.build(ast.value());
    REQUIRE(built.ok());
    Graph graph = std::move(built.value());

    auto transform = make_variable_expansion_transform();
    Context ctx;
    ctx.set("needle.logs_dir", ".needle/meta/logs");

    auto result = transform->apply(graph, ctx);
    REQUIRE(result.ok());

    REQUIRE(graph.find_node("gen")->attrs.get("prompt") ==
            "Emit {{logs_dir}}/build literally");
}
