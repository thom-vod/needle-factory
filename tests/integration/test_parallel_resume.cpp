#include <catch2/catch.hpp>

#include "needle/engine/pipeline_engine.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/event/event_bus.h"
#include "needle/event/collector_event_bus.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "needle/platform/platform.h"

#include <map>
#include <mutex>
#include <set>

using namespace needle;

namespace {

// Per-node handler that counts how many times each node executes and can be
// told to FAIL a given set of node ids. Thread-safe because parallel branches
// run concurrently.
class CountingFailHandler : public Handler {
public:
    explicit CountingFailHandler(const std::string& type) : type_(type) {}
    std::string type_name() const override { return type_; }

    void fail_nodes(std::set<std::string> ids) {
        std::lock_guard<std::mutex> lk(mu_);
        fail_ = std::move(ids);
    }
    int count(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        return counts_[id];
    }

    Result<Outcome> execute(const Node& node, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        bool fail;
        {
            std::lock_guard<std::mutex> lk(mu_);
            counts_[node.id]++;
            fail = fail_.count(node.id) > 0;
        }
        Outcome o;
        o.status = fail ? StageStatus::FAILURE : StageStatus::SUCCESS;
        o.output = (fail ? "FAIL " : "OK ") + node.id;
        return Result<Outcome>::success(std::move(o));
    }

private:
    std::string type_;
    std::mutex mu_;
    std::map<std::string, int> counts_;
    std::set<std::string> fail_;
};

// start -> fork(parallel, wait_all) -> {a1->a2->join, b1->b2->join} -> join -> end
// Mirrors game_design.dot's storyboard_imagery_fan: two-node branches, wait_all.
Graph make_two_node_fanout() {
    std::vector<Node> nodes;
    { Node n; n.id = "start"; n.type = NodeType::START; nodes.push_back(std::move(n)); }
    {
        Node n; n.id = "fork"; n.type = NodeType::PARALLEL;
        n.attrs.set("join_policy", "wait_all");
        nodes.push_back(std::move(n));
    }
    { Node n; n.id = "a1"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "a2"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "b1"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "b2"; n.type = NodeType::CODERGEN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "join"; n.type = NodeType::FAN_IN; nodes.push_back(std::move(n)); }
    { Node n; n.id = "end"; n.type = NodeType::EXIT; nodes.push_back(std::move(n)); }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "fork"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "fork"; e.to = "a1"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "fork"; e.to = "b1"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "a1"; e.to = "a2"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "a2"; e.to = "join"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "b1"; e.to = "b2"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "b2"; e.to = "join"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "join"; e.to = "end"; edges.push_back(std::move(e)); }

    return Graph::make("two_node_fanout", std::move(nodes), std::move(edges));
}

std::shared_ptr<HandlerRegistry> make_registry(std::shared_ptr<CountingFailHandler> codergen) {
    auto registry = std::make_shared<HandlerRegistry>();
    registry->register_handler("start", std::make_shared<CountingFailHandler>("start"));
    registry->register_handler("parallel", std::make_shared<CountingFailHandler>("parallel"));
    registry->register_handler("codergen", std::move(codergen));
    registry->register_handler("fan_in", std::make_shared<CountingFailHandler>("fan_in"));
    registry->register_handler("exit", std::make_shared<CountingFailHandler>("exit"));
    return registry;
}

} // anonymous namespace

// A clean (no-failure) parallel run must execute every branch node exactly
// once. Regression for: after the parallel node completed, the engine followed
// the lexically-first branch edge (fork->a1) via the edge selector and re-ran
// that branch serially in the main loop — duplicating side effects.
TEST_CASE("Parallel: clean fan-out runs each branch exactly once",
          "[integration][parallel][resume]") {
    Graph graph = make_two_node_fanout();
    auto h = std::make_shared<CountingFailHandler>("codergen");  // nothing fails

    PipelineConfig config;
    config.handler_registry = make_registry(h);
    config.checkpoint_writer = std::make_shared<InMemoryCheckpointWriter>();

    PipelineEngine engine(std::move(config));
    Context ctx;
    EventBus bus;
    auto result = engine.run(graph, ctx, bus);

    INFO("run error: " << (result.ok() ? "(ok)" : result.error()));
    CHECK(result.ok());
    CHECK(h->count("a1") == 1);
    CHECK(h->count("a2") == 1);
    CHECK(h->count("b1") == 1);
    CHECK(h->count("b2") == 1);
}

// Partial fan-out failure then resume. Branch A (a1,a2) succeeds; branch B
// (b1,b2) fails at b2, so the wait_all join fails and we checkpoint at `fork`.
// On resume (b2 now succeeds):
//   - branch A must NOT be re-executed (already succeeded — no duplicate work)
//   - branch B must be re-executed (it failed)
//   - the pipeline must complete
TEST_CASE("Parallel: resume reconciles per-branch success/failure",
          "[integration][parallel][resume]") {
    Graph graph = make_two_node_fanout();
    auto cp_writer = std::make_shared<InMemoryCheckpointWriter>();
    std::string logs = platform::temp_dir() + "/needle_parallel_resume_test";

    // First run: b2 fails -> wait_all join fails.
    auto h1 = std::make_shared<CountingFailHandler>("codergen");
    h1->fail_nodes({"b2"});
    {
        PipelineConfig config;
        config.handler_registry = make_registry(h1);
        config.checkpoint_writer = cp_writer;
        config.logs_root = logs;

        PipelineEngine engine(std::move(config));
        Context ctx;
        EventBus bus;
        auto result = engine.run(graph, ctx, bus);
        INFO("first run error: " << (result.ok() ? "(ok)" : result.error()));
        REQUIRE_FALSE(result.ok());
    }

    auto cp1 = cp_writer->load(logs + "/checkpoint.json");
    REQUIRE(cp1.ok());
    // Checkpoint stays at the parallel node, with per-branch status recorded.
    CHECK(cp1.value().current_node == "fork");
    CHECK(cp1.value().context.get("parallel.a1.status") == "SUCCESS");
    CHECK(cp1.value().context.get("parallel.b1.status") == "FAILURE");
    CHECK(h1->count("a1") == 1);
    CHECK(h1->count("b2") == 1);

    // Second run: resume, nothing fails now.
    auto h2 = std::make_shared<CountingFailHandler>("codergen");
    {
        PipelineConfig config;
        config.handler_registry = make_registry(h2);
        config.checkpoint_writer = cp_writer;
        config.logs_root = logs;

        PipelineEngine engine(std::move(config));
        EventBus bus;
        CollectorEventBus collector;
        bus.subscribe([&collector](const PipelineEvent& e) { collector.record(e); });

        auto result = engine.resume(cp1.value(), graph, bus);
        INFO("resume error: " << (result.ok() ? "(ok)" : result.error()));

        bool completed = false;
        for (const auto& e : collector.events()) {
            if (e.type == EventType::PIPELINE_COMPLETED) completed = true;
        }

        CHECK(h2->count("a1") == 0);   // already-succeeded branch not re-run
        CHECK(h2->count("a2") == 0);
        CHECK(h2->count("b1") == 1);   // failed branch re-run
        CHECK(h2->count("b2") == 1);
        CHECK(completed);
        CHECK(result.ok());
    }
}
