#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/backend/process_runner.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include <atomic>

using namespace needle;

TEST_CASE("ToolHandler: success on exit code 0", "[tool_handler]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "build successful";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock->enqueue(resp);

    auto handler = make_tool_handler(mock);

    Node node;
    node.id = "build";
    node.type = NodeType::TOOL;
    node.attrs.set("command", "make -j4");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().output == "build successful");

    // Check context updates
    REQUIRE(result.value().context_updates.count("tool.build.stdout"));
    REQUIRE(result.value().context_updates["tool.build.exit_code"] == "0");
}

TEST_CASE("ToolHandler: failure on non-zero exit code", "[tool_handler]") {
    auto mock = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 1;
    resp.stdout_output = "";
    resp.stderr_output = "error: undefined reference";
    resp.timed_out = false;
    mock->enqueue(resp);

    auto handler = make_tool_handler(mock);

    Node node;
    node.id = "test_tool";
    node.type = NodeType::TOOL;
    node.attrs.set("command", "gcc main.c");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::FAILURE);
    REQUIRE(result.value().context_updates["tool.test_tool.exit_code"] == "1");
}

TEST_CASE("ToolHandler: missing command attribute returns failure", "[tool_handler]") {
    auto mock = std::make_shared<MockProcessRunner>();
    auto handler = make_tool_handler(mock);

    Node node;
    node.id = "no_cmd";
    node.type = NodeType::TOOL;
    // No command attribute set

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("ToolHandler: branch cwd override and parent opt-out", "[tool_handler][worktree]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "ok";
    mock->enqueue(resp);
    mock->enqueue(resp);
    auto handler = make_tool_handler(mock);

    Node branch_node;
    branch_node.id = "tool1";
    branch_node.type = NodeType::TOOL;
    branch_node.attrs.set("command", "echo hi");

    Node parent_node = branch_node;
    parent_node.id = "tool2";
    parent_node.attrs.set("cwd_scope", "parent");

    Context ctx;
    ctx.set("needle.branch.cwd", "/tmp/branch-wt");
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {branch_node, parent_node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, std::string("/tmp/parent"), FidelityMode::FULL, cancelled};

    auto r1 = handler->execute(branch_node, ctx, exec_ctx);
    auto r2 = handler->execute(parent_node, ctx, exec_ctx);
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    auto calls = mock->calls();
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].working_dir == "/tmp/branch-wt");
    REQUIRE(calls[1].working_dir == "/tmp/parent");
}
