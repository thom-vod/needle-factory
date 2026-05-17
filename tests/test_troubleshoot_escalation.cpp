#include <catch2/catch.hpp>

#include "needle/backend/process_runner.h"
#include "needle/engine/auto_troubleshoot.h"
#include "needle/event/event_bus.h"
#include "needle/handlers/interactive_session.h"
#include "needle/model/graph.h"
#include "needle/platform/platform.h"

#include <fstream>
#include <sstream>
#include <vector>

using namespace needle;

namespace {

struct Fixture {
    std::string dir;
    Fixture() {
        dir = platform::temp_dir() + "/needle_ts_escalation_test";
        platform::remove_recursive(dir);
        platform::mkdir_p(dir + "/stages/node");
        std::ofstream cp(dir + "/checkpoint.json");
        cp << "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],"
              "\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},"
              "\"graph_file\":\"\",\"graph_hash\":\"x\"}";
        std::ofstream st(dir + "/stages/node/status.json");
        st << "{\"status\":\"FAILURE\",\"output\":\"boom\"}";
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

class EscalatingRunner : public ProcessRunner {
public:
    EscalatingRunner(std::string session_dir, std::string reason, std::string question)
        : session_dir_(std::move(session_dir))
        , reason_(std::move(reason))
        , question_(std::move(question)) {}

    Result<ProcessResult> run(const std::string&,
                              const std::vector<std::string>&,
                              const std::string&,
                              int,
                              const std::map<std::string, std::string>&,
                              const std::string&,
                              int,
                              std::function<void(const std::string&)> stdout_callback) override {
        platform::mkdir_p(session_dir_);
        std::ofstream esc(session_dir_ + "/escalate.json");
        esc << "{\"reason\":\"" << reason_ << "\","
            << "\"next_question\":\"" << question_ << "\"}";
        ProcessResult result;
        result.exit_code = 0;
        result.stdout_output =
            "agent prelude\n"
            "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
            "\"result\":\"needs operator\",\"total_cost_usd\":0.04}\n"
            "final summary line";
        if (stdout_callback) stdout_callback(result.stdout_output);
        return Result<ProcessResult>::success(result);
    }

private:
    std::string session_dir_;
    std::string reason_;
    std::string question_;
};

} // namespace

TEST_CASE("AutoTroubleshoot bridges escalate marker to report, SSE, and interactive session",
          "[auto_troubleshoot][escalation]") {
    InteractiveSessionRegistry::clear();
    Fixture f;
    const std::string session_id = "fixed-escalation";
    const std::string session_dir = f.dir + "/troubleshoot/session-" + session_id;
    const std::string reason = "parallel race needs operator";
    const std::string question = "Should these branches be serialized?";

    auto runner = std::make_shared<EscalatingRunner>(session_dir, reason, question);
    AutoTroubleshoot ats(runner);

    std::vector<PipelineEvent> events;
    EventBus bus;
    bus.subscribe([&](const PipelineEvent& e) { events.push_back(e); });

    Context ctx;
    ctx.set("needle.project_dir", ".");
    ctx.set("needle.run_id", "run-escalate");
    ctx.set("needle.troubleshoot_session_id", session_id);

    auto result = ats.handle("node", simple_graph(), f.dir, ctx, 1,
                             TroubleshootMode::Diagnose, &bus);

    REQUIRE(result.action == AutoTroubleshootAction::Escalated);
    REQUIRE(result.session_id == session_id);
    REQUIRE(result.message == reason);

    const std::string report = read_file(result.report_path);
    REQUIRE(report.find("outcome: escalated") != std::string::npos);
    REQUIRE(report.find("escalate_reason: \"" + reason + "\"") != std::string::npos);
    REQUIRE(report.find("Escalated: " + reason) != std::string::npos);

    const std::string node_id = "troubleshoot-escalate-" + session_id;
    auto registered = InteractiveSessionRegistry::get(node_id);
    REQUIRE(registered);
    // SPRINT-016 M9 fix: a fresh InteractiveSession is allocated per
    // escalation rather than stomping the run's shared session.
    {
        std::lock_guard<std::mutex> lock(registered->mutex);
        REQUIRE(registered->active);
        REQUIRE(registered->node_id == node_id);
        REQUIRE(registered->opener.find(reason) != std::string::npos);
        REQUIRE(registered->opener.find(question) != std::string::npos);
    }

    bool saw_escalated = false;
    for (const auto& e : events) {
        if (e.type != EventType::TROUBLESHOOT_ACTIVITY) continue;
        if (!e.data.contains("event_type") || e.data["event_type"] != "session_escalated") continue;
        saw_escalated = true;
        const auto& payload = e.data["payload"];
        REQUIRE(payload["reason"] == reason);
        REQUIRE(payload["next_question"] == question);
        REQUIRE(payload["interactive_node_id"] == node_id);
        REQUIRE(payload.value("last_summary", "").find("final summary line") != std::string::npos);
    }
    REQUIRE(saw_escalated);
}
