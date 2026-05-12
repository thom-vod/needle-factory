#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/engine/subgraph_executor.h"
#include "needle/engine/retry_controller.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include "needle/worktree/strategy.h"
#include <atomic>
#include <mutex>

using namespace needle;

namespace {

class StubSubgraphExecutor : public SubgraphExecutor {
public:
    std::mutex mutex;
    int call_count;
    std::map<std::string, std::string> context_updates_to_apply;
    bool received_branch_retry;  // M7: track whether branch retry was passed

    StubSubgraphExecutor() : call_count(0), received_branch_retry(false) {}

    Result<Outcome> execute_subgraph(
        const std::string& /*start_node_id*/,
        const std::string& /*end_node_id*/,
        Context& ctx,
        const ExecutionContext& /*exec_ctx*/) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++call_count;
        }
        ctx.apply_updates(context_updates_to_apply);
        Outcome o;
        o.status = StageStatus::SUCCESS;
        o.context_updates = context_updates_to_apply;
        return Result<Outcome>::success(std::move(o));
    }

    // M7: Overload with branch retry controller
    Result<Outcome> execute_subgraph(
        const std::string& start_node_id,
        const std::string& end_node_id,
        Context& ctx,
        const ExecutionContext& exec_ctx,
        RetryController* branch_retry) override
    {
        if (branch_retry) {
            std::lock_guard<std::mutex> lock(mutex);
            received_branch_retry = true;
        }
        return execute_subgraph(start_node_id, end_node_id, ctx, exec_ctx);
    }
};

Graph make_parallel_graph() {
    std::vector<Node> nodes;

    Node parallel;
    parallel.id = "par";
    parallel.type = NodeType::PARALLEL;
    nodes.push_back(std::move(parallel));

    Node branch_a;
    branch_a.id = "branch_a";
    branch_a.type = NodeType::CODERGEN;
    nodes.push_back(std::move(branch_a));

    Node branch_b;
    branch_b.id = "branch_b";
    branch_b.type = NodeType::CODERGEN;
    nodes.push_back(std::move(branch_b));

    Node fan_in;
    fan_in.id = "merge";
    fan_in.type = NodeType::FAN_IN;
    nodes.push_back(std::move(fan_in));

    std::vector<Edge> edges;

    Edge e1;
    e1.from = "par";
    e1.to = "branch_a";
    edges.push_back(std::move(e1));

    Edge e2;
    e2.from = "par";
    e2.to = "branch_b";
    edges.push_back(std::move(e2));

    Edge e3;
    e3.from = "branch_a";
    e3.to = "merge";
    edges.push_back(std::move(e3));

    Edge e4;
    e4.from = "branch_b";
    e4.to = "merge";
    edges.push_back(std::move(e4));

    return Graph::make("parallel_test", std::move(nodes), std::move(edges));
}

} // anonymous namespace

TEST_CASE("ParallelHandler: spawns threads for each branch", "[parallel_handler]") {
    auto executor = std::make_shared<StubSubgraphExecutor>();
    auto handler = make_parallel_handler(executor, WorktreeConfig{});

    Graph graph = make_parallel_graph();
    const Node* par_node = graph.find_node("par");
    REQUIRE(par_node != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*par_node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);

    // Both branches should have been executed
    REQUIRE(executor->call_count == 2);

    // Branch count should be recorded
    REQUIRE(result.value().context_updates.count("parallel.branch_count"));
    REQUIRE(result.value().context_updates["parallel.branch_count"] == "2");

    // Branch list and join policy should be recorded
    REQUIRE(result.value().context_updates.count("parallel.branches"));
    REQUIRE(result.value().context_updates.count("parallel.join_policy"));
    REQUIRE(result.value().context_updates["parallel.join_policy"] == "wait_all");

    // Branch outputs should be stored
    std::string branches = result.value().context_updates["parallel.branches"];
    REQUIRE(!branches.empty());
    // Both branch_a and branch_b should appear
    REQUIRE(branches.find("branch_a") != std::string::npos);
    REQUIRE(branches.find("branch_b") != std::string::npos);
}

TEST_CASE("ParallelHandler: merges context with namespaced keys", "[parallel_handler]") {
    auto executor = std::make_shared<StubSubgraphExecutor>();
    executor->context_updates_to_apply = {{"result", "done"}};
    auto handler = make_parallel_handler(executor, WorktreeConfig{});

    Graph graph = make_parallel_graph();
    const Node* par_node = graph.find_node("par");
    REQUIRE(par_node != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*par_node, ctx, exec_ctx);
    REQUIRE(result.ok());

    // Should have namespaced keys for both branches
    auto& updates = result.value().context_updates;
    REQUIRE(updates.count("branch_a.result") == 1);
    REQUIRE(updates.count("branch_b.result") == 1);
}

TEST_CASE("ParallelHandler: M7 — per-branch retry controller passed to executor", "[parallel_handler][M7]") {
    auto executor = std::make_shared<StubSubgraphExecutor>();
    auto handler = make_parallel_handler(executor, WorktreeConfig{});

    Graph graph = make_parallel_graph();
    const Node* par_node = graph.find_node("par");
    REQUIRE(par_node != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*par_node, ctx, exec_ctx);
    REQUIRE(result.ok());

    // Both branches should have been called with branch retry controllers
    REQUIRE(executor->received_branch_retry == true);
}

TEST_CASE("ParallelHandler: M15 — BFS fan-in with internal conditional", "[parallel_handler][M15]") {
    // Build a graph where branch_a has an internal conditional (two outgoing edges)
    // Both paths eventually reach the fan-in
    std::vector<Node> nodes;
    { Node n; n.id = "par"; n.type = NodeType::PARALLEL; nodes.push_back(std::move(n)); }
    { Node n; n.id = "br_a"; n.type = NodeType::CONDITIONAL; nodes.push_back(std::move(n)); }
    { Node n; n.id = "a_left"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "a_right"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "br_b"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "merge"; n.type = NodeType::FAN_IN; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "par"; e.to = "br_a"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "par"; e.to = "br_b"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "br_a"; e.to = "a_left"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "br_a"; e.to = "a_right"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "a_left"; e.to = "merge"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "a_right"; e.to = "merge"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "br_b"; e.to = "merge"; edges.push_back(std::move(e)); }

    Graph graph = Graph::make("branching_par", std::move(nodes), std::move(edges));

    auto executor = std::make_shared<StubSubgraphExecutor>();
    auto handler = make_parallel_handler(executor, WorktreeConfig{});

    const Node* par_node = graph.find_node("par");
    REQUIRE(par_node != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*par_node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);

    // Both branches should have been executed
    REQUIRE(executor->call_count == 2);

    // Fan-in target should be "merge"
    REQUIRE(result.value().context_updates.at("parallel.fan_in_target") == "merge");
}

TEST_CASE("ParallelHandler: M13 — fan-in target set for engine skip", "[parallel_handler][M13]") {
    auto executor = std::make_shared<StubSubgraphExecutor>();
    auto handler = make_parallel_handler(executor, WorktreeConfig{});

    Graph graph = make_parallel_graph();
    const Node* par_node = graph.find_node("par");
    REQUIRE(par_node != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*par_node, ctx, exec_ctx);
    REQUIRE(result.ok());

    // Fan-in target should be set so engine can skip past it
    REQUIRE(result.value().context_updates.count("parallel.fan_in_target") == 1);
    REQUIRE(result.value().context_updates.at("parallel.fan_in_target") == "merge");
}
