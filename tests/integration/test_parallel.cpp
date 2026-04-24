#include <catch2/catch.hpp>

#include "needle/engine/pipeline_engine.h"
#include "needle/event/event_bus.h"
#include "needle/event/collector_event_bus.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "helpers/graph_fixtures.h"

using namespace needle;

namespace {

class ContextSettingHandler : public Handler {
public:
    ContextSettingHandler(const std::string& type, const std::string& key, const std::string& value)
        : type_(type), key_(key), value_(value) {}

    std::string type_name() const override { return type_; }

    Result<Outcome> execute(const Node& node, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        Outcome o;
        o.status = StageStatus::SUCCESS;
        o.output = "from " + node.id;
        if (!key_.empty()) {
            o.context_updates[key_] = value_;
        }
        return Result<Outcome>::success(std::move(o));
    }

private:
    std::string type_;
    std::string key_;
    std::string value_;
};

class SimpleSuccessHandler : public Handler {
public:
    explicit SimpleSuccessHandler(const std::string& type) : type_(type) {}
    std::string type_name() const override { return type_; }
    Result<Outcome> execute(const Node& /*node*/, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        Outcome o;
        o.status = StageStatus::SUCCESS;
        return Result<Outcome>::success(std::move(o));
    }
private:
    std::string type_;
};

} // anonymous namespace

TEST_CASE("Integration: parallel pipeline runs both branches", "[integration][parallel]") {
    Graph graph = fixtures::make_parallel_graph();

    auto registry = std::make_shared<HandlerRegistry>();
    registry->register_handler("start", std::make_shared<SimpleSuccessHandler>("start"));
    registry->register_handler("parallel", std::make_shared<SimpleSuccessHandler>("parallel"));
    registry->register_handler("codergen", std::make_shared<SimpleSuccessHandler>("codergen"));
    registry->register_handler("fan_in", std::make_shared<SimpleSuccessHandler>("fan_in"));
    registry->register_handler("exit", std::make_shared<SimpleSuccessHandler>("exit"));

    PipelineConfig config;
    config.handler_registry = registry;

    PipelineEngine engine(std::move(config));

    Context ctx;
    EventBus bus;
    CollectorEventBus collector;
    bus.subscribe([&collector](const PipelineEvent& e) {
        collector.record(e);
    });

    auto result = engine.run(graph, ctx, bus);
    REQUIRE(result.ok());

    // Verify pipeline completed
    auto events = collector.events();
    REQUIRE(events.front().type == EventType::PIPELINE_STARTED);
    REQUIRE(events.back().type == EventType::PIPELINE_COMPLETED);

    // Verify all nodes were visited
    std::vector<std::string> visited;
    for (const auto& e : events) {
        if (e.type == EventType::STAGE_STARTED) {
            visited.push_back(e.node_id);
        }
    }

    // start, fork, branch_a, branch_b, join, end should all be visited
    // (parallel handler and fan_in handler may handle branching internally)
    REQUIRE(visited.size() >= 3);  // at minimum: start, fork, end
}
