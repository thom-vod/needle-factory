#include <catch2/catch.hpp>
#include "needle/backend/cli_backend.h"
#include "needle/backend/process_runner.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include <fstream>
#include <string>
#include <cstdio>
#include <algorithm>
#include "needle/platform/platform.h"

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
    // Simple cleanup for test dirs
    std::remove((path + "/debug.log").c_str());
    std::remove((path + "/session_id").c_str());
    std::remove((path + "/prompt.md").c_str());
    std::remove((path + "/response.md").c_str());
    std::remove((path + "/status.json").c_str());
    platform::remove_dir(path);
}

} // anonymous namespace

TEST_CASE("CLITemplate: claude_default has expected pattern", "[cli_backend]") {
    CLITemplate t = CLITemplate::claude_default();
    REQUIRE(t.provider == "claude");
    REQUIRE(t.command_pattern.find("{model}") != std::string::npos);
    REQUIRE(t.command_pattern.find("--dangerously-skip-permissions") != std::string::npos);
    REQUIRE(t.pipe_prompt_via_stdin == true);
    // Prompt is piped via stdin, not passed as file arg
    REQUIRE(t.command_pattern.find("{prompt_file}") == std::string::npos);
}

TEST_CASE("CLITemplate: codex_default has expected pattern", "[cli_backend]") {
    CLITemplate t = CLITemplate::codex_default();
    REQUIRE(t.provider == "codex");
}

TEST_CASE("CLITemplate: gemini_default has expected pattern", "[cli_backend]") {
    CLITemplate t = CLITemplate::gemini_default();
    REQUIRE(t.provider == "gemini");
}

TEST_CASE("CLIBackend: executes with mock process runner", "[cli_backend]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "generated code here";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "implement";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Write hello world");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_cli_backend";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().output == "generated code here");

    // Verify prompt.md was written (fidelity preamble is prepended before the actual prompt)
    std::string prompt_content = read_file(stage_dir + "/prompt.md");
    REQUIRE(prompt_content.find("Write hello world") != std::string::npos);

    // Verify response.md was written
    std::string response_content = read_file(stage_dir + "/response.md");
    REQUIRE(response_content == "generated code here");

    // Verify the command was called
    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);

    // Cleanup
    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: non-zero exit code returns FAILURE", "[cli_backend]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 1;
    resp.stdout_output = "";
    resp.stderr_output = "error occurred";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "fail_node";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "do something");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_cli_fail";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::FAILURE);

    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: Codex time-only rate limit is detected and preserved",
          "[cli_backend][rate_limit]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 1;
    resp.stdout_output = "";
    resp.stderr_output =
        "ERROR: You've hit your usage limit. Upgrade to Pro "
        "(https://chatgpt.com/explore/pro), visit "
        "https://chatgpt.com/codex/settings/usage to purchase more credits "
        "or try again at 6:26 PM.";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::codex_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "impl";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "implement");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_cli_rl_time_only";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::RETRY);
    REQUIRE(result.value().output.find("Usage limit") != std::string::npos);
    REQUIRE(result.value().output.find("6:26 PM") != std::string::npos);
    REQUIRE(result.value().retry_after_ms > 0);

    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: full-datetime rate limit parses and clamps to 24h",
          "[cli_backend][rate_limit]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 1;
    resp.stdout_output = "";
    // A far-future datetime — the parser must succeed and the wait must be
    // clamped to 24 hours so int diff_s doesn't overflow.
    resp.stderr_output =
        "You've hit your usage limit. try again at Jan 1, 2099 1:41 AM.";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::codex_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "impl";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "implement");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_cli_rl_fulldate";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::RETRY);
    // Parser ran (far exceeds 60s fallback) and clamped (fits in 24h + 5s).
    REQUIRE(result.value().retry_after_ms > 60000);
    REQUIRE(result.value().retry_after_ms <= 86400 * 1000 + 5000);

    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: rate-limit pattern with no parseable time uses 60s default",
          "[cli_backend][rate_limit]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 1;
    resp.stdout_output = "";
    // Rate limit detected, but no "try again in/after/at" clause the parser
    // can interpret. Should still RETRY with the documented 60s fallback.
    resp.stderr_output = "Rate limit exceeded. Please slow down.";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "impl";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "implement");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_cli_rl_nodefault";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::RETRY);
    REQUIRE(result.value().retry_after_ms == 60000);

    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: pipes prompt via stdin when configured", "[cli_backend]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "output";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLITemplate tmpl = CLITemplate::claude_default();
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "stdin_test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Implement feature X");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_stdin_pipe";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    // Fidelity preamble is prepended, so the prompt text is not at position 0
    REQUIRE(calls[0].stdin_data.find("Implement feature X") != std::string::npos);
    // Prompt file path should NOT appear in args
    for (const auto& arg : calls[0].args) {
        REQUIRE(arg.find("prompt.md") == std::string::npos);
    }

    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: no stdin when pipe_prompt_via_stdin is false", "[cli_backend]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLITemplate tmpl;
    tmpl.command_pattern = "echo {prompt_file}";
    tmpl.provider = "test";
    tmpl.pipe_prompt_via_stdin = false;
    CLIBackend backend(tmpl, mock);

    Node node;
    node.id = "codex_test";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Do something");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_no_stdin";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].stdin_data.empty());

    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: selects template by llm_provider attribute", "[cli_backend]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock->enqueue(resp);
    mock->enqueue(resp);

    // Set up multi-template backend: default=claude, codex available
    std::map<std::string, CLITemplate> templates;
    templates["claude"] = CLITemplate::claude_default();
    templates["codex"] = CLITemplate::codex_default();
    CLIBackend backend(CLITemplate::claude_default(), templates, mock);

    // Node with llm_provider=codex should use codex template
    Node node;
    node.id = "codex_node";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "Build something");
    node.attrs.set("llm_provider", "codex");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_provider_select";

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    // Should have used codex command, not claude
    REQUIRE(calls[0].command == "codex");

    // Node without llm_provider should use default (claude)
    rmdir_r(stage_dir);
    Node node2;
    node2.id = "default_node";
    node2.type = NodeType::CODERGEN;
    node2.attrs.set("prompt", "Build something else");

    std::string stage_dir2 = platform::temp_dir() + "/needle_test_provider_default";
    auto result2 = backend.execute(node2, ctx, stage_dir2);
    REQUIRE(result2.ok());

    auto calls2 = mock->calls();
    REQUIRE(calls2.size() == 2);
    REQUIRE(calls2[1].command == "claude");

    rmdir_r(stage_dir2);
}

TEST_CASE("CLIBackend: command wrapper rewrites command, args, env", "[cli_backend][extension]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLIBackend backend(CLITemplate::claude_default(), mock);

    // Install a wrapper that prepends a fake launcher and injects an env var.
    backend.set_command_wrapper([](const std::string& cmd,
                                   const std::vector<std::string>& args,
                                   const Node& node,
                                   const Context& /*ctx*/) {
        WrappedCommand w;
        w.command = "fake-launcher";
        w.args.push_back("--for");
        w.args.push_back(node.id);
        w.args.push_back("--");
        w.args.push_back(cmd);
        w.args.insert(w.args.end(), args.begin(), args.end());
        w.env_overrides["WRAPPER_MARKER"] = "installed";
        return w;
    });

    Node node;
    node.id = "wrapped_node";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "hello");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_cmd_wrapper";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].command == "fake-launcher");

    const auto& call_args = calls[0].args;
    REQUIRE(std::find(call_args.begin(), call_args.end(), "--for") != call_args.end());
    REQUIRE(std::find(call_args.begin(), call_args.end(), "wrapped_node") != call_args.end());
    auto sep_it = std::find(call_args.begin(), call_args.end(), "--");
    REQUIRE(sep_it != call_args.end());
    REQUIRE((sep_it + 1) != call_args.end());
    REQUIRE(*(sep_it + 1) == "claude");

    REQUIRE(calls[0].env_overrides.count("WRAPPER_MARKER") == 1);
    REQUIRE(calls[0].env_overrides.at("WRAPPER_MARKER") == "installed");

    rmdir_r(stage_dir);
}

TEST_CASE("CLIBackend: no wrapper means command passes through unchanged", "[cli_backend][extension]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock->enqueue(resp);

    CLIBackend backend(CLITemplate::claude_default(), mock);

    Node node;
    node.id = "plain_node";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "hi");

    Context ctx;
    std::string stage_dir = platform::temp_dir() + "/needle_test_cmd_wrapper_off";
    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].command == "claude");
    REQUIRE(calls[0].env_overrides.empty());

    rmdir_r(stage_dir);
}

// ─── Fidelity preamble guardrails ──────────────────────────────────────

namespace {

// Run a node at the given fidelity mode and return what got written to
// prompt.md (which is preamble + "---\n\n" + the node's prompt).
std::string capture_preamble_for_mode(const std::string& fidelity_mode) {
    auto mock = std::make_shared<MockProcessRunner>();
    mock->enqueue(ProcessResult{0, "ok", "", false});

    CLIBackend backend(CLITemplate::claude_default(), mock);

    Node node;
    node.id = "n";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "USER_PROMPT_SENTINEL");

    Context ctx;
    ctx.set("needle.fidelity_mode", fidelity_mode);
    ctx.set("needle.goal", "test goal");

    std::string stage_dir = platform::temp_dir() + "/needle_test_guardrails_" + fidelity_mode;
    // Mode strings can contain ':' which is a legal directory char on POSIX
    // but we'll replace just to be safe across platforms.
    std::replace(stage_dir.begin(), stage_dir.end(), ':', '_');

    auto result = backend.execute(node, ctx, stage_dir);
    REQUIRE(result.ok());

    std::string content = read_file(stage_dir + "/prompt.md");
    rmdir_r(stage_dir);
    return content;
}

} // anonymous namespace

TEST_CASE("CLIBackend: fidelity preamble carries package-cache guardrails in every mode",
          "[cli_backend][guardrails]") {
    const std::vector<std::string> modes = {
        "truncate", "compact", "summary:high", "summary:medium", "summary:low", "full"
    };
    for (const auto& mode : modes) {
        SECTION("mode=" + mode) {
            std::string prompt_md = capture_preamble_for_mode(mode);
            // The actual prompt is preserved.
            CHECK(prompt_md.find("USER_PROMPT_SENTINEL") != std::string::npos);
            // The guardrail block is present.
            CHECK(prompt_md.find("## Guardrails") != std::string::npos);
            CHECK(prompt_md.find("~/.nuget/packages") != std::string::npos);
            CHECK(prompt_md.find("TARGETED grep") != std::string::npos);
            // Guardrails sit between the context block and the user prompt
            // (i.e. before the "---" separator the caller injects).
            auto guard_pos = prompt_md.find("## Guardrails");
            auto sep_pos   = prompt_md.find("\n---\n");
            auto user_pos  = prompt_md.find("USER_PROMPT_SENTINEL");
            REQUIRE(guard_pos != std::string::npos);
            REQUIRE(sep_pos   != std::string::npos);
            REQUIRE(user_pos  != std::string::npos);
            CHECK(guard_pos < sep_pos);
            CHECK(sep_pos   < user_pos);
            // Exactly one guardrail block — no double-append regression.
            auto first  = prompt_md.find("## Guardrails");
            auto second = prompt_md.find("## Guardrails", first + 1);
            CHECK(second == std::string::npos);
        }
    }
}

TEST_CASE("CLIBackend: unknown fidelity mode falls back to compact and still emits guardrails",
          "[cli_backend][guardrails]") {
    std::string prompt_md = capture_preamble_for_mode("banana");
    // Compact body markers.
    CHECK(prompt_md.find("## Context") != std::string::npos);
    CHECK(prompt_md.find("### Completed stages") != std::string::npos);
    // Guardrails still appended exactly once.
    CHECK(prompt_md.find("## Guardrails") != std::string::npos);
    CHECK(prompt_md.find("USER_PROMPT_SENTINEL") != std::string::npos);
    auto first  = prompt_md.find("## Guardrails");
    auto second = prompt_md.find("## Guardrails", first + 1);
    CHECK(second == std::string::npos);
}
