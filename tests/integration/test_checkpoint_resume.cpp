#include <catch2/catch.hpp>

#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/engine/pipeline_engine.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/event/event_bus.h"
#include "needle/event/collector_event_bus.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "needle/platform/platform.h"

using namespace needle;

namespace {

// Handler that fails on a specific call number, then succeeds
class FailOnceHandler : public Handler {
public:
    FailOnceHandler(const std::string& type, const std::string& fail_node)
        : type_(type), fail_node_(fail_node) {}

    std::string type_name() const override { return type_; }

    Result<Outcome> execute(const Node& node, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        Outcome o;
        if (node.id == fail_node_ && !already_failed_) {
            already_failed_ = true;
            o.status = StageStatus::FAILURE;
            o.output = "simulated failure";
        } else {
            o.status = StageStatus::SUCCESS;
            o.output = "success from " + type_;
        }
        return Result<Outcome>::success(std::move(o));
    }

private:
    std::string type_;
    std::string fail_node_;
    bool already_failed_ = false;
};

class AlwaysSucceedHandler : public Handler {
public:
    explicit AlwaysSucceedHandler(const std::string& type) : type_(type), count_(0) {}
    std::string type_name() const override { return type_; }

    Result<Outcome> execute(const Node& /*node*/, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        ++count_;
        Outcome o;
        o.status = StageStatus::SUCCESS;
        o.output = "success";
        return Result<Outcome>::success(std::move(o));
    }

    int count() const { return count_; }

private:
    std::string type_;
    int count_;
};

Graph make_checkpoint_graph() {
    std::vector<Node> nodes;
    { Node n; n.id = "start"; n.type = NodeType::START; nodes.push_back(std::move(n)); }
    { Node n; n.id = "step_a"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "step_b"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "exit"; n.type = NodeType::EXIT; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "step_a"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "step_a"; e.to = "step_b"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "step_b"; e.to = "exit"; edges.push_back(std::move(e)); }

    return Graph::make("checkpoint_test", std::move(nodes), std::move(edges));
}

} // anonymous namespace

TEST_CASE("Integration: checkpoint saved on failure, resume completes", "[integration][checkpoint]") {
    Graph graph = make_checkpoint_graph();

    auto cp_writer = std::make_shared<InMemoryCheckpointWriter>();

    // First run: fail on step_b
    {
        auto registry = std::make_shared<HandlerRegistry>();
        registry->register_handler("start", std::make_shared<AlwaysSucceedHandler>("start"));
        registry->register_handler("codergen", std::make_shared<FailOnceHandler>("codergen", "step_b"));
        registry->register_handler("exit", std::make_shared<AlwaysSucceedHandler>("exit"));

        PipelineConfig config;
        config.handler_registry = registry;
        config.checkpoint_writer = cp_writer;
        config.logs_root = platform::temp_dir() + "/needle_cp_test";

        PipelineEngine engine(std::move(config));
        Context ctx;
        EventBus bus;

        auto result = engine.run(graph, ctx, bus);
        REQUIRE_FALSE(result.ok());  // should fail

        // Checkpoint should have been saved
        auto cp = cp_writer->load(platform::temp_dir() + "/needle_cp_test/checkpoint.json");
        REQUIRE(cp.ok());
        // step_a should be in completed_nodes since it succeeded
        bool has_step_a = false;
        for (const auto& n : cp.value().completed_nodes) {
            if (n == "step_a") has_step_a = true;
        }
        REQUIRE(has_step_a);
    }

    // Second run: resume with handlers that succeed
    {
        auto cp = cp_writer->load(platform::temp_dir() + "/needle_cp_test/checkpoint.json");
        REQUIRE(cp.ok());

        auto succeed_handler = std::make_shared<AlwaysSucceedHandler>("codergen");
        auto registry = std::make_shared<HandlerRegistry>();
        registry->register_handler("start", std::make_shared<AlwaysSucceedHandler>("start"));
        registry->register_handler("codergen", succeed_handler);
        registry->register_handler("exit", std::make_shared<AlwaysSucceedHandler>("exit"));

        PipelineConfig config;
        config.handler_registry = registry;
        config.checkpoint_writer = cp_writer;
        config.logs_root = platform::temp_dir() + "/needle_cp_test";

        PipelineEngine engine(std::move(config));
        EventBus bus;

        CollectorEventBus collector;
        bus.subscribe([&collector](const PipelineEvent& e) {
            collector.record(e);
        });

        auto result = engine.resume(cp.value(), graph, bus);
        REQUIRE(result.ok());

        // Verify pipeline completed
        auto events = collector.events();
        bool completed = false;
        for (const auto& e : events) {
            if (e.type == EventType::PIPELINE_COMPLETED) {
                completed = true;
            }
        }
        REQUIRE(completed);
    }
}
