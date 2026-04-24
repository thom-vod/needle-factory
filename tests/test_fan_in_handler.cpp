#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/backend/backend.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include <atomic>

using namespace needle;

namespace {

// Stub backend that records what it was called with
class StubBackend : public Backend {
public:
    std::string name() const override { return "stub"; }

    Result<Outcome> execute(const Node& node, Context& /*ctx*/,
                            const std::string& /*stage_dir*/) override {
        last_prompt = node.prompt();
        call_count++;
        Outcome o;
        o.status = StageStatus::SUCCESS;
        o.output = consensus_output;
        return Result<Outcome>::success(std::move(o));
    }

    std::string last_prompt;
    std::string consensus_output = "merged result";
    int call_count = 0;
};

} // anonymous namespace

TEST_CASE("FanInHandler: passes through cleanly", "[fan_in_handler]") {
    auto handler = make_fan_in_handler();

    Node node;
    node.id = "merge";
    node.type = NodeType::FAN_IN;

    Context ctx;
    ctx.set("parallel.branch_a.result", "done");
    ctx.set("parallel.branch_b.result", "done");
    ctx.set("parallel.branch_count", "2");

    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
}

TEST_CASE("FanInHandler: type_name is fan_in", "[fan_in_handler]") {
    auto handler = make_fan_in_handler();
    REQUIRE(handler->type_name() == "fan_in");
}

TEST_CASE("FanInHandler: consensus calls backend with branch outputs", "[fan_in_handler]") {
    auto backend = std::make_shared<StubBackend>();
    backend->consensus_output = "the consensus result";
    auto handler = make_fan_in_handler(backend);

    Node node;
    node.id = "merge";
    node.type = NodeType::FAN_IN;
    node.attrs.set("prompt", "Pick the best approach from these branches.");

    Context ctx;
    ctx.set("parallel.join_policy", "consensus");
    ctx.set("parallel.branches", "alpha,beta");
    ctx.set("parallel.alpha.output", "Alpha says: use approach A");
    ctx.set("parallel.alpha.status", "SUCCESS");
    ctx.set("parallel.beta.output", "Beta says: use approach B");
    ctx.set("parallel.beta.status", "SUCCESS");
    ctx.set("parallel.branch_count", "2");

    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);

    // Backend should have been called once
    REQUIRE(backend->call_count == 1);

    // Prompt should contain the fan-in prompt and both branch outputs
    REQUIRE(backend->last_prompt.find("Pick the best approach") != std::string::npos);
    REQUIRE(backend->last_prompt.find("Alpha says: use approach A") != std::string::npos);
    REQUIRE(backend->last_prompt.find("Beta says: use approach B") != std::string::npos);
    REQUIRE(backend->last_prompt.find("### Branch: alpha") != std::string::npos);
    REQUIRE(backend->last_prompt.find("### Branch: beta") != std::string::npos);

    // Consensus result should be in context_updates
    REQUIRE(result.value().context_updates.count("parallel.consensus.result"));
    REQUIRE(result.value().context_updates["parallel.consensus.result"] == "the consensus result");
}

TEST_CASE("FanInHandler: consensus with null backend falls back to passthrough", "[fan_in_handler]") {
    auto handler = make_fan_in_handler(nullptr);

    Node node;
    node.id = "merge";
    node.type = NodeType::FAN_IN;

    Context ctx;
    ctx.set("parallel.join_policy", "consensus");
    ctx.set("parallel.branches", "a,b");
    ctx.set("parallel.a.output", "output a");
    ctx.set("parallel.b.output", "output b");

    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    // Should be passthrough, no consensus.result
    REQUIRE(result.value().context_updates.count("parallel.consensus.result") == 0);
}

TEST_CASE("FanInHandler: non-consensus policy ignores backend", "[fan_in_handler]") {
    auto backend = std::make_shared<StubBackend>();
    auto handler = make_fan_in_handler(backend);

    Node node;
    node.id = "merge";
    node.type = NodeType::FAN_IN;

    Context ctx;
    ctx.set("parallel.join_policy", "wait_all");
    ctx.set("parallel.branches", "a,b");

    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    // Backend should NOT have been called
    REQUIRE(backend->call_count == 0);
}
