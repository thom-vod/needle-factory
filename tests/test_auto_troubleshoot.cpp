#include <catch2/catch.hpp>

#include "needle/backend/process_runner.h"
#include "needle/engine/auto_troubleshoot.h"
#include "needle/engine/remediation_plan.h"
#include "needle/model/graph.h"
#include "needle/platform/platform.h"

#include <algorithm>
#include <fstream>
#include <sstream>

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

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void require_session_layout(const Fixture& f, const AutoTroubleshootResult& result) {
    REQUIRE_FALSE(result.session_id.empty());
    const std::string session_dir = f.dir + "/troubleshoot/session-" + result.session_id;
    REQUIRE(platform::is_directory(session_dir));
    REQUIRE(platform::file_exists(session_dir + "/events.ndjson"));
    REQUIRE(platform::file_exists(session_dir + "/recovery.md"));
    REQUIRE(platform::is_directory(session_dir + "/snapshot"));
    REQUIRE(platform::file_exists(session_dir + "/agent.stdout.log"));
    REQUIRE(platform::file_exists(session_dir + "/agent.stderr.log"));
    REQUIRE(result.report_path == session_dir + "/recovery.md");

    const std::string report = read_file(result.report_path);
    REQUIRE(report.find("schema_version: 2") != std::string::npos);
    REQUIRE(report.find("session_id: \"" + result.session_id + "\"") != std::string::npos);
    REQUIRE(report.find("run_id: \"needle_auto_ts_test\"") != std::string::npos);
    REQUIRE(report.find("tier: diagnose") != std::string::npos);
    REQUIRE(report.find("trust: snapshot") != std::string::npos);
    REQUIRE(report.find("failed_node: \"node\"") != std::string::npos);
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
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = R"({"type":"result","subtype":"success","is_error":false,"result":"done"})";
    mock->enqueue(resp);
    Context ctx;
    ctx.set("needle.project_dir", ".");
    AutoTroubleshoot ats(mock);
    auto result1 = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Diagnose);
    REQUIRE(result1.action == AutoTroubleshootAction::Resumed);
    require_session_layout(f, result1);

    auto result2 = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Diagnose);
    REQUIRE(result2.action == AutoTroubleshootAction::Escalated);
    require_session_layout(f, result2);
    REQUIRE(ctx.get("troubleshoot.attempts.node") == "1");
}

TEST_CASE("AutoTroubleshoot skips off mode", "[auto_troubleshoot]") {
    Fixture f;
    Context ctx;
    AutoTroubleshoot ats;
    auto result = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Off);
    REQUIRE(result.action == AutoTroubleshootAction::Skipped);
}

TEST_CASE("AutoTroubleshoot dispatches full mode to agent", "[auto_troubleshoot]") {
    Fixture f;
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = R"({"type":"result","subtype":"success","is_error":false,"result":"done"})";
    mock->enqueue(resp);
    Context ctx;
    ctx.set("needle.project_dir", ".");
    AutoTroubleshoot ats(mock);
    auto result = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Full);
    REQUIRE(result.action == AutoTroubleshootAction::Resumed);
    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(),
                      "--dangerously-skip-permissions") != calls[0].args.end());
}

TEST_CASE("Node troubleshoot false override can be represented", "[auto_troubleshoot]") {
    Graph g = simple_graph();
    Node* n = g.mutable_node("node");
    REQUIRE(n != nullptr);
    n->attrs.set("troubleshoot", "false");
    REQUIRE(n->attrs.get("troubleshoot") == "false");
}
