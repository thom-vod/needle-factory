#include <catch2/catch.hpp>

#include "needle/backend/process_runner.h"
#include "needle/engine/auto_troubleshoot.h"
#include "needle/engine/remediation_plan.h"
#include "needle/model/graph.h"
#include "needle/platform/platform.h"
#include "needle/worktree/strategy.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

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

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
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

void write_file(const std::string& path, const std::string& value) {
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) platform::mkdir_p(path.substr(0, slash));
    std::ofstream out(path);
    out << value;
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
    resp.stdout_output =
        R"({"type":"system","subtype":"init","session_id":"abc","model":"claude-opus-4-7"})" "\n"
        R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_1","name":"Read","input":{"file_path":"status.json"}}]}})" "\n"
        R"({"type":"result","subtype":"success","is_error":false,"result":"done","total_cost_usd":0.12,"num_turns":1})";
    mock->enqueue(resp);
    Context ctx;
    ctx.set("needle.project_dir", ".");
    AutoTroubleshoot ats(mock);
    auto result1 = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Diagnose);
    REQUIRE(result1.action == AutoTroubleshootAction::Resumed);
    require_session_layout(f, result1);
    {
        const std::string events_path = f.dir + "/troubleshoot/session-" + result1.session_id + "/events.ndjson";
        auto lines = read_lines(events_path);
        REQUIRE(lines.size() == 4);
        REQUIRE(lines[0].find(R"("type":"system")") != std::string::npos);
        REQUIRE(lines[1].find(R"("tool_use")") != std::string::npos);
        REQUIRE(lines[2] == lines[3]);
        REQUIRE(lines[2].find(R"("total_cost_usd":0.12)") != std::string::npos);
    }

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
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    std::string root = platform::temp_dir() + "/needle_auto_ts_full_" + std::to_string(getpid());
    platform::remove_recursive(root);
    std::string project = root + "/project";
    std::string run_dir = project + "/.needle/flow";
    platform::mkdir_p(run_dir + "/stages/node");
    write_file(run_dir + "/checkpoint.json",
               "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},\"graph_file\":\"\",\"graph_hash\":\"x\"}");
    write_file(run_dir + "/stages/node/status.json", "{\"status\":\"FAILURE\"}");
    write_file(project + "/flow.dot", "digraph flow { node; }\n");
    REQUIRE(std::system(("cd '" + project + "' && git init -q && git config user.email needle-test@example.com && git config user.name 'Needle Test' && git config commit.gpgsign false && git add flow.dot && git commit -qm initial").c_str()) == 0);

    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = R"({"type":"result","subtype":"success","is_error":false,"result":"done"})";
    mock->enqueue(resp);
    Context ctx;
    ctx.set("needle.project_dir", project);
    ctx.set("needle.graph_path", project + "/flow.dot");
    ctx.set("needle.run_id", "run-full");
    AutoTroubleshoot ats(mock);
    auto result = ats.handle("node", simple_graph(), run_dir, ctx, 1, TroubleshootMode::Full);
    REQUIRE(result.action == AutoTroubleshootAction::Resumed);
    std::string session_dir = run_dir + "/troubleshoot/session-" + result.session_id;
    REQUIRE(platform::file_exists(session_dir + "/worktree/branch.txt"));
    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(),
                      "--dangerously-skip-permissions") != calls[0].args.end());
    REQUIRE(calls[0].working_dir.find("troubleshoot-wt-run-full") != std::string::npos);
    REQUIRE(TroubleshootWorktree::discard(project, "run-full").ok());
    platform::remove_recursive(root);
#endif
}

TEST_CASE("AutoTroubleshoot captures snapshot for tweak mode", "[auto_troubleshoot]") {
    std::string root = platform::temp_dir() + "/needle_auto_ts_snapshot";
    platform::remove_recursive(root);
    std::string project = root + "/project";
    std::string run_dir = project + "/.needle/flow";
    platform::mkdir_p(run_dir + "/stages/node");
    write_file(run_dir + "/checkpoint.json",
               "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},\"graph_file\":\"\",\"graph_hash\":\"x\"}");
    write_file(run_dir + "/stages/node/status.json", "{\"status\":\"FAILURE\"}");
    write_file(run_dir + "/stages/node/prompt.md", "prompt before\n");
    write_file(project + "/flow.dot", "digraph flow { node; }\n");

    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = R"({"type":"result","subtype":"success","is_error":false,"result":"done"})";
    mock->enqueue(resp);
    Context ctx;
    ctx.set("needle.project_dir", project);
    ctx.set("needle.graph_path", project + "/flow.dot");
    ctx.set("needle.run_id", "run-snapshot");
    AutoTroubleshoot ats(mock);
    auto result = ats.handle("node", simple_graph(), run_dir, ctx, 1, TroubleshootMode::Tweak);
    REQUIRE(result.action == AutoTroubleshootAction::Resumed);
    std::string snapshot_dir = run_dir + "/troubleshoot/session-" + result.session_id + "/snapshot";
    REQUIRE(platform::file_exists(snapshot_dir + "/flow.dot"));
    REQUIRE(platform::file_exists(snapshot_dir + "/prompt.md.node"));
    REQUIRE(platform::file_exists(snapshot_dir + "/manifest.json"));
    platform::remove_recursive(root);
}

TEST_CASE("Node troubleshoot false override can be represented", "[auto_troubleshoot]") {
    Graph g = simple_graph();
    Node* n = g.mutable_node("node");
    REQUIRE(n != nullptr);
    n->attrs.set("troubleshoot", "false");
    REQUIRE(n->attrs.get("troubleshoot") == "false");
}
