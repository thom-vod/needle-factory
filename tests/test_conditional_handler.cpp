#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/model/outcome.h"
#include "needle/event/event_bus.h"
#include <atomic>

using namespace needle;

TEST_CASE("ConditionalHandler: evaluates against parent FAILURE outcome", "[conditional]") {
    auto handler = make_conditional_handler();

    // Build a conditional node with two outgoing edges
    Node cond_node;
    cond_node.id = "check";
    cond_node.type = NodeType::CONDITIONAL;

    Node target_fail;
    target_fail.id = "handle_failure";
    target_fail.type = NodeType::EXIT;

    Node target_success;
    target_success.id = "handle_success";
    target_success.type = NodeType::EXIT;

    Edge e_fail;
    e_fail.from = "check";
    e_fail.to = "handle_failure";
    e_fail.attrs.set("label", "failure_path");
    e_fail.attrs.set("condition", "outcome=FAILURE");

    Edge e_success;
    e_success.from = "check";
    e_success.to = "handle_success";
    e_success.attrs.set("label", "success_path");
    e_success.attrs.set("condition", "outcome=SUCCESS");

    Graph graph = Graph::make("test", {cond_node, target_fail, target_success}, {e_fail, e_success});

    Context ctx;
    // M3 fix: engine sets this after the prior node's FAILURE
    ctx.set("needle.last_outcome.status", "FAILURE");

    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(cond_node, ctx, exec_ctx);
    REQUIRE(result.ok());
    // The failure_path edge should be selected
    REQUIRE(result.value().preferred_label == "failure_path");
}

TEST_CASE("ConditionalHandler: missing prior outcome defaults to SUCCESS", "[conditional]") {
    auto handler = make_conditional_handler();

    Node cond_node;
    cond_node.id = "check";
    cond_node.type = NodeType::CONDITIONAL;

    Node target_success;
    target_success.id = "handle_success";
    target_success.type = NodeType::EXIT;

    Edge e_success;
    e_success.from = "check";
    e_success.to = "handle_success";
    e_success.attrs.set("label", "success_path");
    e_success.attrs.set("condition", "outcome=SUCCESS");

    Graph graph = Graph::make("test", {cond_node, target_success}, {e_success});

    Context ctx;
    // No needle.last_outcome.status set — should default to SUCCESS

    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(cond_node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().preferred_label == "success_path");
}
