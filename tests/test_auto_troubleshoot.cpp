#include <catch2/catch.hpp>

#include "needle/backend/process_runner.h"
#include "needle/engine/auto_troubleshoot.h"
#include "needle/engine/troubleshoot_backup.h"
#include "needle/model/graph.h"
#include "needle/platform/platform.h"
#include "needle/troubleshoot/stream_parser.h"

#include <algorithm>
#include <functional>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace needle;

namespace needle {

void process_agent_stream_line(const std::string& line,
                               TroubleshootStreamParser& parser,
                               std::ofstream& events_out,
                               EventBus* event_bus,
                               const std::string& run_id,
                               const std::string& session_id,
                               const std::string& node_id,
                               TroubleshootMode mode);

std::string create_auto_troubleshoot_session_dir_for_test(
    const std::string& run_dir,
    std::string& session_id,
    const std::function<std::string()>& make_session_id);

} // namespace needle

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
    REQUIRE(platform::file_exists(session_dir + "/agent.stdout.log"));
    REQUIRE(platform::file_exists(session_dir + "/agent.stderr.log"));
    REQUIRE(result.report_path == session_dir + "/recovery.md");

    const std::string report = read_file(result.report_path);
    REQUIRE(report.find("schema_version: 2") != std::string::npos);
    REQUIRE(report.find("session_id: \"" + result.session_id + "\"") != std::string::npos);
    REQUIRE(report.find("run_id: \"needle_auto_ts_test\"") != std::string::npos);
    // SPRINT-016: trust field removed from schema.
    REQUIRE(report.find("trust:") == std::string::npos);
    REQUIRE(report.find("backup_branch:") != std::string::npos);
    REQUIRE(report.find("tier: diagnose") != std::string::npos);
    REQUIRE(report.find("failed_node: \"node\"") != std::string::npos);
}

void write_file(const std::string& path, const std::string& value) {
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) platform::mkdir_p(path.substr(0, slash));
    std::ofstream out(path);
    out << value;
}

class DirtyingProcessRunner : public ProcessRunner {
public:
    DirtyingProcessRunner(std::string path, std::string value)
        : path_(std::move(path)), value_(std::move(value)) {}

    Result<ProcessResult> run(
        const std::string&,
        const std::vector<std::string>&,
        const std::string&,
        int,
        const std::map<std::string, std::string>& = {},
        const std::string& = "",
        int = 0,
        std::function<void(const std::string&)> stdout_callback = nullptr) override {
        write_file(path_, value_);
        ProcessResult resp;
        resp.exit_code = 7;
        resp.stderr_output = "agent failed after edit";
        resp.stdout_output = R"({"type":"result","subtype":"error","is_error":true,"result":"failed"})";
        if (stdout_callback) stdout_callback(resp.stdout_output + "\n");
        return Result<ProcessResult>::success(std::move(resp));
    }

private:
    std::string path_;
    std::string value_;
};

} // namespace

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
    // SPRINT-016 B3 fix: Diagnose returns Reported (not Resumed) so the engine
    // does not retry the failed stage.
    REQUIRE(result1.action == AutoTroubleshootAction::Reported);
    require_session_layout(f, result1);
    {
        const std::string events_path = f.dir + "/troubleshoot/session-" + result1.session_id + "/events.ndjson";
        auto lines = read_lines(events_path);
        // SPRINT-016 M12 fix: events.ndjson writes one line per source line,
        // not one line per parsed event. The result line previously appeared
        // twice (session_completed + cost_update both parsed from it).
        REQUIRE(lines.size() == 3);
        REQUIRE(lines[0].find(R"("type":"system")") != std::string::npos);
        REQUIRE(lines[1].find(R"("tool_use")") != std::string::npos);
        REQUIRE(lines[2].find(R"("total_cost_usd":0.12)") != std::string::npos);
    }

    auto result2 = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Diagnose);
    REQUIRE(result2.action == AutoTroubleshootAction::Escalated);
    require_session_layout(f, result2);
    REQUIRE(ctx.get("troubleshoot.attempts.node") == "1");
}

TEST_CASE("AutoTroubleshoot records raw text-only stream lines without SSE events", "[auto_troubleshoot]") {
    Fixture f;
    const std::string events_path = f.dir + "/events.ndjson";
    std::ofstream events_out(events_path, std::ios::app);
    TroubleshootStreamParser parser;
    EventBus bus;
    int emitted = 0;
    bus.subscribe([&](const PipelineEvent&) {
        emitted++;
    });

    process_agent_stream_line(
        R"({"type":"assistant","message":{"content":[{"type":"text","text":"hello"}]}})",
        parser, events_out, &bus, "run-1", "session-1", "node", TroubleshootMode::Diagnose);
    events_out.close();

    auto lines = read_lines(events_path);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].find(R"("text":"hello")") != std::string::npos);
    REQUIRE(emitted == 0);
}

TEST_CASE("AutoTroubleshoot skips off mode", "[auto_troubleshoot]") {
    Fixture f;
    Context ctx;
    AutoTroubleshoot ats;
    auto result = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Off);
    REQUIRE(result.action == AutoTroubleshootAction::Skipped);
}

TEST_CASE("AutoTroubleshoot rerolls canonical id on session directory collision",
          "[auto_troubleshoot]") {
    Fixture f;
    int calls = 0;
    auto generator = [&]() {
        ++calls;
        return calls == 1 ? "2026-05-18T12-00-00Z-abcd"
                          : "2026-05-18T12-00-00Z-1234";
    };
    platform::mkdir_p(f.dir + "/troubleshoot/session-2026-05-18T12-00-00Z-abcd");

    std::string session_id;
    std::string session_dir =
        create_auto_troubleshoot_session_dir_for_test(f.dir, session_id, generator);

    const std::regex pattern(R"(^[0-9-]+T[0-9-]+Z-[0-9a-f]{4}$)");
    REQUIRE(session_id == "2026-05-18T12-00-00Z-1234");
    REQUIRE(std::regex_match(session_id, pattern));
    REQUIRE(session_dir == f.dir + "/troubleshoot/session-" + session_id);
    REQUIRE(calls == 2);
}

TEST_CASE("AutoTroubleshoot flags audited writes outside allowed roots", "[auto_troubleshoot]") {
    Fixture f;
    const std::string outside_path = platform::path_join(platform::temp_dir(),
                                                        "needle_auto_ts_outside_file");
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;

    nlohmann::json tool_block;
    tool_block["type"] = "tool_use";
    tool_block["id"] = "toolu_outside";
    tool_block["name"] = "Edit";
    tool_block["input"]["file_path"] = outside_path;

    nlohmann::json assistant_line;
    assistant_line["type"] = "assistant";
    assistant_line["message"]["content"] = nlohmann::json::array({tool_block});

    resp.stdout_output =
        assistant_line.dump() + "\n" +
        R"({"type":"result","subtype":"success","is_error":false,"result":"done"})";
    mock->enqueue(resp);

    Context ctx;
    ctx.set("needle.project_dir", f.dir);
    AutoTroubleshoot ats(mock);
    auto result = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Diagnose);

    REQUIRE(result.action == AutoTroubleshootAction::Skipped);
    REQUIRE_FALSE(result.report_path.empty());
    const std::string report = read_file(result.report_path);
    REQUIRE(report.find("outcome: failed_hook_violation") != std::string::npos);
    REQUIRE(report.find("## Security audit") != std::string::npos);
    REQUIRE(report.find("Edit " + outside_path + " (tool_use_id=toolu_outside)") != std::string::npos);
}

TEST_CASE("AutoTroubleshoot records failed-agent tracked edits for rollback", "[auto_troubleshoot]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    std::string root = platform::temp_dir() + "/needle_auto_ts_failed_dirty_" + std::to_string(getpid());
    platform::remove_recursive(root);
    std::string project = root + "/project";
    std::string run_dir = project + "/.needle/flow";
    platform::mkdir_p(run_dir + "/stages/node");
    write_file(run_dir + "/checkpoint.json",
               "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},\"graph_file\":\"\",\"graph_hash\":\"x\"}");
    write_file(run_dir + "/stages/node/status.json", "{\"status\":\"FAILURE\"}");
    write_file(project + "/flow.dot", "digraph flow { node; }\n");
    write_file(project + "/tracked.txt", "base\n");
    REQUIRE(std::system(("cd '" + project + "' && git init -q && git config user.email needle-test@example.com && git config user.name 'Needle Test' && git config commit.gpgsign false && git add flow.dot tracked.txt && git commit -qm initial").c_str()) == 0);

    auto runner = std::make_shared<DirtyingProcessRunner>(project + "/tracked.txt",
                                                          "agent dirty\n");
    Context ctx;
    ctx.set("needle.project_dir", project);
    ctx.set("needle.graph_path", project + "/flow.dot");
    ctx.set("needle.run_id", "run-failed-dirty");
    AutoTroubleshoot ats(runner);
    auto result = ats.handle("node", simple_graph(), run_dir, ctx, 1, TroubleshootMode::Tweak);

    REQUIRE(result.action == AutoTroubleshootAction::Skipped);
    std::string session_dir = run_dir + "/troubleshoot/session-" + result.session_id;
    REQUIRE(platform::file_exists(session_dir + "/agent-modified.txt"));
    const std::string agent_modified = read_file(session_dir + "/agent-modified.txt");
    REQUIRE(agent_modified.find("tracked.txt") != std::string::npos);

    auto rollback = TroubleshootBackup::rollback(project, session_dir);
    REQUIRE(rollback.ok());
    REQUIRE(read_file(project + "/tracked.txt") == "base\n");
    platform::remove_recursive(root);
#endif
}

TEST_CASE("AutoTroubleshoot reports hook violation even when agent fails", "[auto_troubleshoot]") {
    Fixture f;
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 42;
    resp.stderr_output = "agent failed";

    nlohmann::json tool_block;
    tool_block["type"] = "tool_use";
    tool_block["id"] = "toolu_passwd";
    tool_block["name"] = "Write";
    tool_block["input"]["file_path"] = "/etc/passwd";

    nlohmann::json assistant_line;
    assistant_line["type"] = "assistant";
    assistant_line["message"]["content"] = nlohmann::json::array({tool_block});

    resp.stdout_output = R"({"type":"result","subtype":"error","is_error":true,"result":"failed"})";
    mock->enqueue(resp);

    Context ctx;
    ctx.set("needle.project_dir", f.dir);
    ctx.set("needle.troubleshoot_session_id", "failed-hook");
    const std::string session_dir = f.dir + "/troubleshoot/session-failed-hook";
    platform::mkdir_p(session_dir);
    write_file(session_dir + "/events.ndjson", assistant_line.dump() + "\n");
    AutoTroubleshoot ats(mock);
    auto result = ats.handle("node", simple_graph(), f.dir, ctx, 1, TroubleshootMode::Diagnose);

    REQUIRE(result.action == AutoTroubleshootAction::Skipped);
    REQUIRE_FALSE(result.report_path.empty());
    const std::string report = read_file(result.report_path);
    REQUIRE(report.find("outcome: failed_hook_violation") != std::string::npos);
    REQUIRE(report.find("outcome: failed_agent") == std::string::npos);
    REQUIRE(report.find("## Security audit") != std::string::npos);
    REQUIRE(report.find("Write /etc/passwd (tool_use_id=toolu_passwd)") != std::string::npos);
}

TEST_CASE("AutoTroubleshoot dispatches full mode to agent with backup branch", "[auto_troubleshoot]") {
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
    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    // SPRINT-016 spec revision: no --dangerously-skip-permissions in any tier.
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(),
                      "--dangerously-skip-permissions") == calls[0].args.end());
    // Full mode now uses --permission-mode default with broader allow-list.
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(),
                      "--permission-mode") != calls[0].args.end());
    // Backup branch was created (Phase 2 wires this; placeholder check OK).
    std::string session_dir = run_dir + "/troubleshoot/session-" + result.session_id;
    REQUIRE(platform::file_exists(session_dir + "/backup-base.txt"));
    platform::remove_recursive(root);
#endif
}

TEST_CASE("AutoTroubleshoot captures backup branch for tweak mode", "[auto_troubleshoot]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    std::string root = platform::temp_dir() + "/needle_auto_ts_backup_" + std::to_string(getpid());
    platform::remove_recursive(root);
    std::string project = root + "/project";
    std::string run_dir = project + "/.needle/flow";
    platform::mkdir_p(run_dir + "/stages/node");
    write_file(run_dir + "/checkpoint.json",
               "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},\"graph_file\":\"\",\"graph_hash\":\"x\"}");
    write_file(run_dir + "/stages/node/status.json", "{\"status\":\"FAILURE\"}");
    write_file(run_dir + "/stages/node/prompt.md", "prompt before\n");
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
    ctx.set("needle.run_id", "run-backup");
    AutoTroubleshoot ats(mock);
    auto result = ats.handle("node", simple_graph(), run_dir, ctx, 1, TroubleshootMode::Tweak);
    REQUIRE(result.action == AutoTroubleshootAction::Resumed);
    std::string session_dir = run_dir + "/troubleshoot/session-" + result.session_id;
    REQUIRE(platform::file_exists(session_dir + "/backup-base.txt"));
    REQUIRE(platform::file_exists(session_dir + "/backup-branch.txt"));
    REQUIRE(platform::file_exists(session_dir + "/pre-untracked.txt"));
    platform::remove_recursive(root);
#endif
}

TEST_CASE("Node troubleshoot false override can be represented", "[auto_troubleshoot]") {
    Graph g = simple_graph();
    Node* n = g.mutable_node("node");
    REQUIRE(n != nullptr);
    n->attrs.set("troubleshoot", "false");
    REQUIRE(n->attrs.get("troubleshoot") == "false");
}
