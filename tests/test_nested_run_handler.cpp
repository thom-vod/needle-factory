#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/backend/process_runner.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include <atomic>
#include <fstream>
#include "needle/platform/platform.h"

using namespace needle;

namespace {

void write_test_dot(const std::string& path) {
    std::ofstream out(path);
    out << "digraph T { start [shape=Mdiamond] exit [shape=Msquare] start -> exit }";
}

} // anonymous namespace

TEST_CASE("NestedRunHandler: type_name is nested_run", "[nested_run_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_nested_run_handler(runner);
    REQUIRE(handler->type_name() == "nested_run");
}

TEST_CASE("NestedRunHandler: missing dot_file returns failure", "[nested_run_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_nested_run_handler(runner);

    Node node;
    node.id = "run1";
    node.type = NodeType::CODERGEN;

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(!result.ok());
}

TEST_CASE("NestedRunHandler: recursion depth limit", "[nested_run_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_nested_run_handler(runner);

    // Create a temporary DOT file
    std::string dot_path = platform::temp_dir() + "/needle_test_nested.dot";
    write_test_dot(dot_path);

    Node node;
    node.id = "run2";
    node.type = NodeType::CODERGEN;
    node.attrs.set("dot_file", dot_path);
    node.attrs.set("max_depth", "3");

    Context ctx;
    ctx.set("needle.depth", "3"); // Already at max depth

    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::FAILURE);
    REQUIRE(result.value().output.find("recursion depth limit") != std::string::npos);

    std::remove(dot_path.c_str());
}

TEST_CASE("NestedRunHandler: successful nested run", "[nested_run_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 0;
    resp.timed_out = false;
    resp.stdout_output = "Pipeline completed";
    runner->enqueue(resp);

    auto handler = make_nested_run_handler(runner);

    std::string dot_path = platform::temp_dir() + "/needle_test_nested2.dot";
    write_test_dot(dot_path);

    Node node;
    node.id = "run3";
    node.type = NodeType::CODERGEN;
    node.attrs.set("dot_file", dot_path);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().context_updates["nested_run.run3.success"] == "true");
    REQUIRE(result.value().context_updates["nested_run.run3.exit_code"] == "0");

    // Verify needle was called with correct args
    auto calls = runner->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].command == "needle");

    // Should have --var needle.depth=1
    bool found_depth = false;
    for (size_t i = 0; i < calls[0].args.size(); ++i) {
        if (calls[0].args[i] == "needle.depth=1") {
            found_depth = true;
            break;
        }
    }
    REQUIRE(found_depth);

    std::remove(dot_path.c_str());
}

TEST_CASE("NestedRunHandler: failed nested run", "[nested_run_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 1;
    resp.timed_out = false;
    resp.stdout_output = "Pipeline failed: stage failed: build";
    runner->enqueue(resp);

    auto handler = make_nested_run_handler(runner);

    std::string dot_path = platform::temp_dir() + "/needle_test_nested3.dot";
    write_test_dot(dot_path);

    Node node;
    node.id = "run4";
    node.type = NodeType::CODERGEN;
    node.attrs.set("dot_file", dot_path);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::FAILURE);
    REQUIRE(result.value().context_updates["nested_run.run4.success"] == "false");

    std::remove(dot_path.c_str());
}
