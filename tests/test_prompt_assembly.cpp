#include <catch2/catch.hpp>

#include "needle/backend/cli_backend.h"
#include "needle/backend/process_runner.h"
#include "needle/model/context.h"
#include "needle/model/graph.h"
#include "needle/platform/platform.h"

#include <fstream>

using namespace needle;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Prompt assembly: critique class excludes implementation trailers", "[prompt_assembly]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    Node node;
    node.id = "crit";
    node.type = NodeType::CODERGEN;
    node.attrs.set("class", "critique");
    node.attrs.set("agent", "gemini");
    node.attrs.set("use_skills", "true");
    node.attrs.set("prompt", "Review this patch.");

    Context ctx;
    ctx.set("human.gate.feedback", "Please fix this.");
    std::string stage = platform::temp_dir() + "/needle_prompt_critique";

    CLIBackend backend(CLITemplate::claude_default(),
        {{"claude", CLITemplate::claude_default()}, {"gemini", CLITemplate::gemini_default()}}, mock);
    REQUIRE(backend.execute(node, ctx, stage).ok());
    std::string prompt = read_file(stage + "/prompt.md");
    REQUIRE(prompt.find("## Available Skills") == std::string::npos);
    REQUIRE(prompt.find("Feedback from reviewer:") == std::string::npos);
}

TEST_CASE("Prompt assembly: apply_feedback injects feedback and implementation workflow", "[prompt_assembly]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);

    Node node;
    node.id = "apply";
    node.type = NodeType::CODERGEN;
    node.attrs.set("class", "apply_feedback");
    node.attrs.set("agent", "claude");
    node.attrs.set("prompt", "Apply changes.");

    Context ctx;
    ctx.set("human.gate.feedback", "Tighten tests");
    std::string stage = platform::temp_dir() + "/needle_prompt_apply_feedback";

    CLIBackend backend(CLITemplate::claude_default(),
        {{"claude", CLITemplate::claude_default()}}, mock);
    REQUIRE(backend.execute(node, ctx, stage).ok());
    std::string prompt = read_file(stage + "/prompt.md");
    REQUIRE(prompt.find("Feedback from reviewer: Tighten tests") != std::string::npos);
    REQUIRE(prompt.find("## Workflow Instructions") != std::string::npos);
}
