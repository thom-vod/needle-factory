#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/engine/subgraph_executor.h"
#include "needle/engine/retry_controller.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include <atomic>

using namespace needle;

namespace {

// Stub that succeeds after N attempts by setting a goal gate
class CountingSubgraphExecutor : public SubgraphExecutor {
public:
    int succeed_after;
    int call_count;
    std::string goal_node_id;

    CountingSubgraphExecutor(int succeed_after_n, const std::string& goal_id)
        : succeed_after(succeed_after_n)
        , call_count(0)
        , goal_node_id(goal_id) {}

    Result<Outcome> execute_subgraph(
        const std::string& /*start_node_id*/,
        const std::string& /*end_node_id*/,
        Context& ctx,
        const ExecutionContext& /*exec_ctx*/) override
    {
        ++call_count;
        Outcome o;
        if (call_count >= succeed_after) {
            // Set goal gate satisfied
            ctx.set("goal." + goal_node_id, "satisfied");
            o.status = StageStatus::SUCCESS;
        } else {
            o.status = StageStatus::SUCCESS; // subgraph itself succeeds
        }
        return Result<Outcome>::success(std::move(o));
    }
};

// Stub that never satisfies goals but always succeeds
class NeverSatisfiesExecutor : public SubgraphExecutor {
public:
    int call_count;
    NeverSatisfiesExecutor() : call_count(0) {}

    Result<Outcome> execute_subgraph(
        const std::string& /*start_node_id*/,
        const std::string& /*end_node_id*/,
        Context& /*ctx*/,
        const ExecutionContext& /*exec_ctx*/) override
    {
        ++call_count;
        Outcome o;
        o.status = StageStatus::SUCCESS;
        return Result<Outcome>::success(std::move(o));
    }
};

Graph make_loop_graph(const std::string& goal_node_id) {
    std::vector<Node> nodes;

    Node manager;
    manager.id = "manager";
    manager.type = NodeType::MANAGER_LOOP;
    manager.attrs.set("max_iterations", "10");
    nodes.push_back(std::move(manager));

    Node work;
    work.id = "work";
    work.type = NodeType::CODERGEN;
    work.attrs.set("goal_gate", "true");
    nodes.push_back(std::move(work));

    (void)goal_node_id;

    std::vector<Edge> edges;

    Edge e1;
    e1.from = "manager";
    e1.to = "work";
    edges.push_back(std::move(e1));

    Edge e2;
    e2.from = "work";
    e2.to = "manager";
    edges.push_back(std::move(e2));

    return Graph::make("loop_test", std::move(nodes), std::move(edges));
}

} // anonymous namespace

TEST_CASE("ManagerLoopHandler: succeeds after N iterations", "[manager_loop]") {
    auto executor = std::make_shared<CountingSubgraphExecutor>(3, "work");
    auto handler = make_manager_loop_handler(executor);

    Graph graph = make_loop_graph("work");
    const Node* manager = graph.find_node("manager");
    REQUIRE(manager != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*manager, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(executor->call_count == 3);
}

TEST_CASE("ManagerLoopHandler: fails after max iterations", "[manager_loop]") {
    // Never satisfies goal
    auto executor = std::make_shared<CountingSubgraphExecutor>(100, "work");
    auto handler = make_manager_loop_handler(executor);

    Graph graph = make_loop_graph("work");
    // Override max_iterations to small value
    for (auto& n : graph.mutable_nodes()) {
        if (n.id == "manager") {
            n.attrs.set("max_iterations", "5");
        }
    }
    const Node* manager = graph.find_node("manager");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*manager, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::FAILURE);
    REQUIRE(executor->call_count == 5);
}

TEST_CASE("ManagerLoopHandler: M12 — external goal gate doesn't prevent loop termination", "[manager_loop][M12]") {
    // Build a graph where:
    // - manager -> work_in_loop (goal_gate=true) -> manager (loop back)
    // - There's also an external node with goal_gate=true that was NEVER executed
    // Pre-M12: the loop would never terminate because the external goal gate is unsatisfied
    // Post-M12: the loop only checks goal gates within the loop body

    std::vector<Node> nodes;
    {
        Node n; n.id = "manager"; n.type = NodeType::MANAGER_LOOP;
        n.attrs.set("max_iterations", "10");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "work_in_loop"; n.type = NodeType::CODERGEN;
        n.attrs.set("goal_gate", "true");
        nodes.push_back(std::move(n));
    }
    {
        // External node with goal_gate=true — NOT part of the loop body
        Node n; n.id = "external_goal"; n.type = NodeType::CODERGEN;
        n.attrs.set("goal_gate", "true");
        nodes.push_back(std::move(n));
    }

    std::vector<Edge> edges;
    { Edge e; e.from = "manager"; e.to = "work_in_loop"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "work_in_loop"; e.to = "manager"; edges.push_back(std::move(e)); }
    // external_goal has no connection to the loop
    { Edge e; e.from = "external_goal"; e.to = "manager"; edges.push_back(std::move(e)); }

    Graph graph = Graph::make("scoped_goal_test", std::move(nodes), std::move(edges));

    // Executor that satisfies work_in_loop's goal on iteration 2
    // but NEVER satisfies external_goal
    auto executor = std::make_shared<CountingSubgraphExecutor>(2, "work_in_loop");
    auto handler = make_manager_loop_handler(executor);

    const Node* manager = graph.find_node("manager");
    REQUIRE(manager != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*manager, ctx, exec_ctx);
    REQUIRE(result.ok());
    // M12: Loop should terminate successfully because only work_in_loop's goal matters
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(executor->call_count == 2);
}
