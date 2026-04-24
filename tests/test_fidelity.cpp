#include <catch2/catch.hpp>
#include "needle/model/fidelity.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/backend/cli_backend.h"
#include "needle/backend/process_runner.h"
#include "needle/platform/platform.h"
#include <fstream>
#include <string>
#include <cstdio>

using namespace needle;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    return content;
}

void rmdir_r(const std::string& path) {
    std::remove((path + "/prompt.md").c_str());
    std::remove((path + "/response.md").c_str());
    std::remove((path + "/status.json").c_str());
    std::remove((path + "/session_id").c_str());
    std::remove((path + "/debug.log").c_str());
    platform::remove_dir(path);
}

} // anonymous namespace

// ============================================================
// FidelityMode enum and string conversions
// ============================================================

TEST_CASE("fidelity_from_string: all valid modes", "[fidelity]") {
    REQUIRE(fidelity_from_string("truncate") == FidelityMode::TRUNCATE);
    REQUIRE(fidelity_from_string("compact") == FidelityMode::COMPACT);
    REQUIRE(fidelity_from_string("summary:low") == FidelityMode::SUMMARY_LOW);
    REQUIRE(fidelity_from_string("summary_low") == FidelityMode::SUMMARY_LOW);
    REQUIRE(fidelity_from_string("summary:medium") == FidelityMode::SUMMARY_MEDIUM);
    REQUIRE(fidelity_from_string("summary_medium") == FidelityMode::SUMMARY_MEDIUM);
    REQUIRE(fidelity_from_string("summary:high") == FidelityMode::SUMMARY_HIGH);
    REQUIRE(fidelity_from_string("summary_high") == FidelityMode::SUMMARY_HIGH);
    REQUIRE(fidelity_from_string("full") == FidelityMode::FULL);
}

TEST_CASE("fidelity_from_string: unknown string defaults to compact", "[fidelity]") {
    REQUIRE(fidelity_from_string("bogus") == FidelityMode::COMPACT);
    REQUIRE(fidelity_from_string("") == FidelityMode::COMPACT);
}

TEST_CASE("to_string round-trips for all FidelityModes", "[fidelity]") {
    REQUIRE(to_string(FidelityMode::TRUNCATE) == "truncate");
    REQUIRE(to_string(FidelityMode::COMPACT) == "compact");
    REQUIRE(to_string(FidelityMode::SUMMARY_LOW) == "summary:low");
    REQUIRE(to_string(FidelityMode::SUMMARY_MEDIUM) == "summary:medium");
    REQUIRE(to_string(FidelityMode::SUMMARY_HIGH) == "summary:high");
    REQUIRE(to_string(FidelityMode::FULL) == "full");
}

// ============================================================
// resolve_fidelity precedence: edge > node > graph > default
// ============================================================

TEST_CASE("resolve_fidelity: edge fidelity takes precedence over node and graph", "[fidelity]") {
    AttributeMap gattrs;
    gattrs.set("default_fidelity", "full");

    Node n;
    n.id = "test";
    n.type = NodeType::CODERGEN;
    n.attrs.set("fidelity", "summary:high");

    Edge e;
    e.from = "start";
    e.to = "test";
    e.attrs.set("fidelity", "truncate");

    Graph graph = Graph::make("test", {n}, {e}, gattrs);
    FidelityMode result = resolve_fidelity(&e, n, graph);
    REQUIRE(result == FidelityMode::TRUNCATE);
}

TEST_CASE("resolve_fidelity: node fidelity used when edge has none", "[fidelity]") {
    AttributeMap gattrs;
    gattrs.set("default_fidelity", "full");

    Node n;
    n.id = "test";
    n.type = NodeType::CODERGEN;
    n.attrs.set("fidelity", "compact");

    Edge e;
    e.from = "start";
    e.to = "test";
    // No fidelity on edge

    Graph graph = Graph::make("test", {n}, {e}, gattrs);
    FidelityMode result = resolve_fidelity(&e, n, graph);
    REQUIRE(result == FidelityMode::COMPACT);
}

TEST_CASE("resolve_fidelity: graph default used when edge and node have none", "[fidelity]") {
    AttributeMap gattrs;
    gattrs.set("default_fidelity", "summary:medium");

    Node n;
    n.id = "test";
    n.type = NodeType::CODERGEN;
    // No fidelity on node

    Graph graph = Graph::make("test", {n}, {}, gattrs);
    FidelityMode result = resolve_fidelity(nullptr, n, graph);
    REQUIRE(result == FidelityMode::SUMMARY_MEDIUM);
}

TEST_CASE("resolve_fidelity: defaults to compact when nothing is set", "[fidelity]") {
    Node n;
    n.id = "test";
    n.type = NodeType::CODERGEN;

    Graph graph = Graph::make("test", {n}, {}, AttributeMap());
    FidelityMode result = resolve_fidelity(nullptr, n, graph);
    REQUIRE(result == FidelityMode::COMPACT);
}

TEST_CASE("resolve_fidelity: null edge is handled gracefully", "[fidelity]") {
    Node n;
    n.id = "test";
    n.type = NodeType::CODERGEN;
    n.attrs.set("fidelity", "full");

    Graph graph = Graph::make("test", {n}, {}, AttributeMap());
    FidelityMode result = resolve_fidelity(nullptr, n, graph);
    REQUIRE(result == FidelityMode::FULL);
}

// ============================================================
// Preamble content tests (via CLIBackend mock execution)
// ============================================================

TEST_CASE("fidelity preamble: truncate mode has goal and current stage", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "implement";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Write code");
    node.attrs.set("label", "Implementation");

    Context ctx;
    ctx.set("needle.fidelity_mode", "truncate");
    ctx.set("needle.goal", "Build a web server");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_truncate";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt.find("## Context") != std::string::npos);
    REQUIRE(prompt.find("Goal: Build a web server") != std::string::npos);
    REQUIRE(prompt.find("Current stage: Implementation") != std::string::npos);
    REQUIRE(prompt.find("Write code") != std::string::npos);
    // Truncate mode should NOT have "Completed stages" section
    REQUIRE(prompt.find("Completed stages") == std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: compact mode includes completed stage outputs", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "validate";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Validate the code");
    node.attrs.set("label", "Validation");

    Context ctx;
    ctx.set("needle.fidelity_mode", "compact");
    ctx.set("needle.goal", "Build a web server");
    ctx.set("codergen.implement.output", "Created main.cpp with HTTP handling");
    ctx.set("codergen.design.results", "Architecture document completed");
    ctx.set("var.language", "C++");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_compact";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt.find("## Context") != std::string::npos);
    REQUIRE(prompt.find("Goal: Build a web server") != std::string::npos);
    REQUIRE(prompt.find("### Completed stages") != std::string::npos);
    REQUIRE(prompt.find("codergen.implement.output") != std::string::npos);
    REQUIRE(prompt.find("codergen.design.results") != std::string::npos);
    // var.language should NOT appear in compact mode (only .output/.results keys)
    REQUIRE(prompt.find("var.language") == std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: compact mode truncates long values to 200 chars", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "review";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Review");
    node.attrs.set("label", "Review");

    // Create a long output value
    std::string long_output(500, 'X');
    Context ctx;
    ctx.set("needle.fidelity_mode", "compact");
    ctx.set("needle.goal", "Test");
    ctx.set("codergen.big.output", long_output);

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_compact_trunc";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt.find("codergen.big.output") != std::string::npos);
    REQUIRE(prompt.find("...") != std::string::npos);
    // The 500-char string should be truncated, so the full string should NOT appear
    REQUIRE(prompt.find(long_output) == std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: summary:high includes non-internal keys", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Test");
    node.attrs.set("label", "Test");

    Context ctx;
    ctx.set("needle.fidelity_mode", "summary:high");
    ctx.set("needle.goal", "Build something");
    ctx.set("var.language", "C++");
    ctx.set("codergen.impl.output", "Some code");
    ctx.set("internal.hidden", "should not appear");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_summary_high";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt.find("### Prior stage outputs") != std::string::npos);
    REQUIRE(prompt.find("var.language") != std::string::npos);
    REQUIRE(prompt.find("codergen.impl.output") != std::string::npos);
    // Internal and needle keys should be excluded
    REQUIRE(prompt.find("internal.hidden") == std::string::npos);
    REQUIRE(prompt.find("needle.fidelity_mode") == std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: summary:medium includes vars and outputs only", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Test");
    node.attrs.set("label", "Test");

    Context ctx;
    ctx.set("needle.fidelity_mode", "summary:medium");
    ctx.set("needle.goal", "Build something");
    ctx.set("var.language", "C++");
    ctx.set("codergen.impl.output", "Some code");
    ctx.set("other.random.key", "should not appear");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_summary_medium";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt.find("var.language") != std::string::npos);
    REQUIRE(prompt.find("codergen.impl.output") != std::string::npos);
    // Random keys should NOT appear in summary:medium
    REQUIRE(prompt.find("other.random.key") == std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: summary:low includes only var keys", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Test");
    node.attrs.set("label", "Test");

    Context ctx;
    ctx.set("needle.fidelity_mode", "summary:low");
    ctx.set("needle.goal", "Build something");
    ctx.set("var.language", "C++");
    ctx.set("var.framework", "Boost");
    ctx.set("codergen.impl.output", "Some code");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_summary_low";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt.find("Prior stages completed") != std::string::npos);
    REQUIRE(prompt.find("var.language") != std::string::npos);
    REQUIRE(prompt.find("var.framework") != std::string::npos);
    // Non-var keys should NOT appear in summary:low
    REQUIRE(prompt.find("codergen.impl.output") == std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: full mode includes everything", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Test");
    node.attrs.set("label", "Test");

    Context ctx;
    ctx.set("needle.fidelity_mode", "full");
    ctx.set("needle.goal", "Build everything");
    ctx.set("var.language", "C++");
    ctx.set("codergen.impl.output", "Some code output");
    ctx.set("other.key", "some value");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_full";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt.find("## Full Context") != std::string::npos);
    REQUIRE(prompt.find("Goal: Build everything") != std::string::npos);
    REQUIRE(prompt.find("var.language") != std::string::npos);
    REQUIRE(prompt.find("codergen.impl.output") != std::string::npos);
    REQUIRE(prompt.find("other.key") != std::string::npos);
    // needle.* keys should be excluded even in full mode
    REQUIRE(prompt.find("needle.fidelity_mode") == std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: unknown mode falls back to compact", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Test");
    node.attrs.set("label", "Test");

    Context ctx;
    ctx.set("needle.fidelity_mode", "nonexistent_mode");
    ctx.set("needle.goal", "Test fallback");
    ctx.set("codergen.step.output", "step output");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_unknown";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    // Should fall back to compact, which has "Completed stages"
    REQUIRE(prompt.find("### Completed stages") != std::string::npos);
    REQUIRE(prompt.find("codergen.step.output") != std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: default mode when context has no fidelity set", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Test");
    node.attrs.set("label", "Test");

    Context ctx;
    // No needle.fidelity_mode set -- should default to compact

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_default";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    // Compact mode preamble
    REQUIRE(prompt.find("## Context") != std::string::npos);
    REQUIRE(prompt.find("### Completed stages") != std::string::npos);

    rmdir_r(stage_dir);
}

TEST_CASE("fidelity preamble: separator between preamble and prompt", "[fidelity][preamble]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "MARKER_PROMPT_START");
    node.attrs.set("label", "Test");

    Context ctx;
    ctx.set("needle.fidelity_mode", "truncate");
    ctx.set("needle.goal", "Test separation");

    std::string stage_dir = platform::temp_dir() + "/needle_test_fidelity_separator";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string prompt = read_file(stage_dir + "/prompt.md");
    // The preamble and prompt are separated by "---"
    size_t separator_pos = prompt.find("---\n\n");
    size_t marker_pos = prompt.find("MARKER_PROMPT_START");
    REQUIRE(separator_pos != std::string::npos);
    REQUIRE(marker_pos != std::string::npos);
    // Prompt comes after the separator
    REQUIRE(marker_pos > separator_pos);

    rmdir_r(stage_dir);
}

// ============================================================
// Context::all() method
// ============================================================

TEST_CASE("Context::all returns all key-value pairs", "[context]") {
    Context ctx;
    ctx.set("a", "1");
    ctx.set("b", "2");
    ctx.set("c", "3");

    const auto& all = ctx.all();
    REQUIRE(all.size() == 3);
    REQUIRE(all.at("a") == "1");
    REQUIRE(all.at("b") == "2");
    REQUIRE(all.at("c") == "3");
}

TEST_CASE("Context::all returns empty map for empty context", "[context]") {
    Context ctx;
    const auto& all = ctx.all();
    REQUIRE(all.empty());
}
