#include <catch2/catch.hpp>

#include "needle/engine/auto_troubleshoot.h"
#include "needle/engine/remediation_plan.h"
#include "needle/model/graph.h"
#include "needle/platform/platform.h"

#include <fstream>

using namespace needle;

namespace {

struct Fixture {
    std::string dir;
    Fixture() {
        dir = platform::temp_dir() + "/needle_auto_ts_test";
        platform::remove_recursive(dir);
        platform::mkdir_p(dir + "/stages/node");
        std::ofstream cp(dir + "/checkpoint.json");
        cp << "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},\"graph_file\":\"\",\"graph_hash\":\"x\"}";
        std::ofstream st(dir + "/stages/node/status.json");
        st << "{\"status\":\"FAILURE\",\"timeout_kind\":\"idle\"}";
    }
    ~Fixture() { platform::remove_recursive(dir); }
};

Graph simple_graph() {
    Node s{"start", NodeType::START, AttributeMap()};
    Node n{"node", NodeType::CODERGEN, AttributeMap()};
    Node e{"exit", NodeType::EXIT, AttributeMap()};
    std::vector<Node> nodes = {s, n, e};
    std::vector<Edge> edges = {
        {"start", "node", AttributeMap()},
        {"node", "exit", AttributeMap()},
    };
    return Graph::make("g", std::move(nodes), std::move(edges));
}

} // namespace

TEST_CASE("Remediation planner maps failure kinds", "[auto_troubleshoot]") {
    DiagnosisReport r;
    r.kind = FailureKind::WallClockWithProgress;
    auto p = plan_remediation(r, "node", "exit", "summary:high");
    REQUIRE(p.type == RemediationPlan::Type::MarkSuccessAdvance);

    r.kind = FailureKind::PromptBlowup;
    p = plan_remediation(r, "node", "exit", "summary:high");
    REQUIRE(p.type == RemediationPlan::Type::ResetWithLowerFidelity);
    REQUIRE(p.fidelity_override == "summary:medium");

    r.kind = FailureKind::RolePromptConflict;
    p = plan_remediation(r, "node", "exit", "summary:high");
    REQUIRE(p.type == RemediationPlan::Type::EscalateToOperator);
}

TEST_CASE("AutoTroubleshoot enforces retry cap", "[auto_troubleshoot]") {
    Fixture f;
    Context ctx;
    AutoTroubleshoot ats;
    auto result1 = ats.handle("node", simple_graph(), f.dir, ctx, 1);
    REQUIRE((result1.action == AutoTroubleshootAction::Resumed ||
             result1.action == AutoTroubleshootAction::Escalated));

    auto result2 = ats.handle("node", simple_graph(), f.dir, ctx, 1);
    REQUIRE(result2.action == AutoTroubleshootAction::Escalated);
    REQUIRE(ctx.get("troubleshoot.attempts.node") == "1");
}

TEST_CASE("Node troubleshoot false override can be represented", "[auto_troubleshoot]") {
    Graph g = simple_graph();
    Node* n = g.mutable_node("node");
    REQUIRE(n != nullptr);
    n->attrs.set("troubleshoot", "false");
    REQUIRE(n->attrs.get("troubleshoot") == "false");
}
