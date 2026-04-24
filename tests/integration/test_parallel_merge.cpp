// Regression test for the parallel fan-in merge bug.
//
// Before the fix, a pipeline with shape=trapezium fan-in + prompt= would
// complete with fan_in.output = "fan-in merge complete" and the fan-in's
// own prompt silently dropped, because fan-in ran inside each branch
// before parallel.join_policy / parallel.branches were populated.
//
// After the fix:
//   - branches stop before the fan-in (inclusive_end=false)
//   - parallel.<branch>.output is the real branch work, not the fallback
//   - fan-in runs once in the parent context with all parallel.* state set
//   - the fan-in's prompt is actually sent to the backend with branch
//     outputs appended under "## Branch Outputs"
//   - codergen.<fan_in_id>.output is set with the merge result

#include <catch2/catch.hpp>

#include "needle/engine/pipeline_engine.h"
#include "needle/event/event_bus.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "needle/handlers/all_handlers.h"
#include "needle/backend/backend.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"

using namespace needle;

namespace {

// A handler that returns a deterministic, per-node output, and sets a
// context_update so apply_updates can verify propagation.
class ScriptedOutputHandler : public Handler {
public:
    std::string type_name() const override { return "codergen"; }
    Result<Outcome> execute(const Node& node, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        Outcome o;
        o.status = StageStatus::SUCCESS;
        o.output = node.id + " produced this";
        o.context_updates["codergen." + node.id + ".output"] = o.output;
        return Result<Outcome>::success(std::move(o));
    }
};

// Records the single prompt sent via backend->execute and returns a fixed
// merge result. Used to verify the fan-in handler calls the backend with
// the right prompt shape.
class RecordingBackend : public Backend {
public:
    std::string name() const override { return "recording-stub"; }
    Result<Outcome> execute(const Node& node, Context& /*ctx*/,
                            const std::string& /*stage_dir*/) override {
        last_prompt = node.prompt();
        last_node_id = node.id;
        ++call_count;
        Outcome o;
        o.status = StageStatus::SUCCESS;
        o.output = merge_output;
        return Result<Outcome>::success(std::move(o));
    }

    std::string last_prompt;
    std::string last_node_id;
    std::string merge_output = "MERGED OUTPUT";
    int call_count = 0;
};

class NoopHandler : public Handler {
public:
    explicit NoopHandler(const std::string& type) : type_(type) {}
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

Graph make_fan_in_merge_graph(const std::string& join_policy,
                                const std::string& fan_in_prompt) {
    std::vector<Node> nodes;
    {
        Node n; n.id = "start"; n.type = NodeType::START;
        n.attrs.set("shape", "Mdiamond");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "fork"; n.type = NodeType::PARALLEL;
        n.attrs.set("shape", "component");
        n.attrs.set("join_policy", join_policy);
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "branch_a"; n.type = NodeType::CODERGEN;
        n.attrs.set("shape", "box");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "branch_b"; n.type = NodeType::CODERGEN;
        n.attrs.set("shape", "box");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "join"; n.type = NodeType::FAN_IN;
        n.attrs.set("shape", "trapezium");
        n.attrs.set("prompt", fan_in_prompt);
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "end"; n.type = NodeType::EXIT;
        n.attrs.set("shape", "Msquare");
        nodes.push_back(std::move(n));
    }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "fork"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "fork"; e.to = "branch_a"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "fork"; e.to = "branch_b"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "branch_a"; e.to = "join"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "branch_b"; e.to = "join"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "join"; e.to = "end"; edges.push_back(std::move(e)); }

    return Graph::make("parallel_merge", std::move(nodes), std::move(edges));
}

struct PipelineFixture {
    std::shared_ptr<RecordingBackend> backend = std::make_shared<RecordingBackend>();
    std::shared_ptr<HandlerRegistry> registry = std::make_shared<HandlerRegistry>();

    PipelineFixture() {
        registry->register_handler("start", std::make_shared<NoopHandler>("start"));
        registry->register_handler("exit", std::make_shared<NoopHandler>("exit"));
        registry->register_handler("codergen", std::make_shared<ScriptedOutputHandler>());
        // parallel is re-registered by PipelineEngine constructor with itself as executor.
        registry->register_handler("parallel", std::make_shared<NoopHandler>("parallel"));
        registry->register_handler("fan_in", make_fan_in_handler(backend));
    }

    Result<void> run(const Graph& graph, Context& ctx, EventBus& bus) {
        PipelineConfig config;
        config.handler_registry = registry;
        config.cli_backend = backend;
        config.auto_status = false;  // no logs_root, skip file writes
        PipelineEngine engine(std::move(config));
        return engine.run(graph, ctx, bus);
    }
};

} // anonymous namespace

TEST_CASE("Integration: fan-in runs its prompt with real branch outputs (default policy)",
          "[integration][parallel][fan_in]") {
    PipelineFixture fx;
    Graph graph = make_fan_in_merge_graph("wait_all",
        "Merge both drafts into one unified result.");

    Context ctx;
    EventBus bus;
    REQUIRE(fx.run(graph, ctx, bus).ok());

    // Backend should have been called exactly once — for the fan-in merge.
    REQUIRE(fx.backend->call_count == 1);
    REQUIRE(fx.backend->last_node_id == "join");

    // The prompt sent to the backend must include the fan-in node's prompt
    // AND the real branch outputs under the "## Branch Outputs" header —
    // NOT the "fan-in merge complete" placeholder.
    REQUIRE(fx.backend->last_prompt.find("Merge both drafts") != std::string::npos);
    REQUIRE(fx.backend->last_prompt.find("## Branch Outputs") != std::string::npos);
    REQUIRE(fx.backend->last_prompt.find("branch_a produced this") != std::string::npos);
    REQUIRE(fx.backend->last_prompt.find("branch_b produced this") != std::string::npos);
    REQUIRE(fx.backend->last_prompt.find("fan-in merge complete") == std::string::npos);

    // Branch outputs must carry the real work-node output, not the fan-in
    // fallback string (this was bug #3 — branches' tail used to be the fan-in).
    REQUIRE(ctx.get("parallel.branch_a.output") == "branch_a produced this");
    REQUIRE(ctx.get("parallel.branch_b.output") == "branch_b produced this");
    REQUIRE(ctx.get("parallel.branch_a.status") == "SUCCESS");
    REQUIRE(ctx.get("parallel.branch_b.status") == "SUCCESS");

    // codergen.<fan_in_id>.output must be set so downstream nodes reading
    // $context.codergen.join.output see the merge result (bug #5).
    REQUIRE(ctx.get("codergen.join.output") == "MERGED OUTPUT");
}

TEST_CASE("Integration: consensus fan-in populates both parallel.consensus.result and codergen.<id>.output",
          "[integration][parallel][fan_in][consensus]") {
    PipelineFixture fx;
    Graph graph = make_fan_in_merge_graph("consensus",
        "Reach consensus between the two drafts.");

    Context ctx;
    EventBus bus;
    REQUIRE(fx.run(graph, ctx, bus).ok());

    REQUIRE(fx.backend->call_count == 1);
    REQUIRE(fx.backend->last_prompt.find("Reach consensus") != std::string::npos);
    REQUIRE(fx.backend->last_prompt.find("branch_a produced this") != std::string::npos);
    REQUIRE(fx.backend->last_prompt.find("branch_b produced this") != std::string::npos);

    // Both keys should hold the merge result — parallel.consensus.result for
    // back-compat with older templates, and codergen.<fan_in_id>.output for
    // the standard $context pattern.
    REQUIRE(ctx.get("parallel.consensus.result") == "MERGED OUTPUT");
    REQUIRE(ctx.get("codergen.join.output") == "MERGED OUTPUT");
}

TEST_CASE("Integration: fan-in without a prompt still passes through cleanly",
          "[integration][parallel][fan_in]") {
    PipelineFixture fx;
    Graph graph = make_fan_in_merge_graph("wait_all", /*fan_in_prompt=*/"");

    Context ctx;
    EventBus bus;
    REQUIRE(fx.run(graph, ctx, bus).ok());

    // With no prompt on the fan-in, backend should NOT be called — default
    // pass-through path applies.
    REQUIRE(fx.backend->call_count == 0);

    // But branch outputs still carry real work, not the fan-in placeholder.
    REQUIRE(ctx.get("parallel.branch_a.output") == "branch_a produced this");
    REQUIRE(ctx.get("parallel.branch_b.output") == "branch_b produced this");
}
