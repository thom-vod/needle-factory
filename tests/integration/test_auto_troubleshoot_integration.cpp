#include <catch2/catch.hpp>

#include "needle/engine/auto_troubleshoot.h"
#include "needle/backend/process_runner.h"
#include "needle/model/context.h"
#include "needle/model/graph.h"
#include "needle/platform/platform.h"

#include <fstream>

using namespace needle;

TEST_CASE("AutoTroubleshoot writes recovery report", "[integration][auto_troubleshoot]") {
    std::string run_dir = platform::temp_dir() + "/needle_auto_ts_integration";
    platform::remove_recursive(run_dir);
    platform::mkdir_p(run_dir + "/stages/node");

    std::ofstream cp(run_dir + "/checkpoint.json");
    cp << "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},\"graph_file\":\"\",\"graph_hash\":\"x\"}";
    std::ofstream st(run_dir + "/stages/node/status.json");
    st << "{\"status\":\"FAILURE\",\"timeout_kind\":\"wall_clock\",\"git_state\":{\"commits_added\":[{\"hash\":\"abc\",\"subject\":\"x\"}],\"files_added_untracked\":[],\"files_modified_uncommitted\":[]}}";

    Node s{"start", NodeType::START, AttributeMap()};
    Node n{"node", NodeType::CODERGEN, AttributeMap()};
    Node e{"exit", NodeType::EXIT, AttributeMap()};
    std::vector<Node> nodes = {s, n, e};
    std::vector<Edge> edges = {{"start", "node", AttributeMap()}, {"node", "exit", AttributeMap()}};
    Graph g = Graph::make("g", std::move(nodes), std::move(edges));

    Context ctx;
    AutoTroubleshoot ats;
    auto res = ats.handle("node", g, run_dir, ctx, 1, TroubleshootMode::Off);
    REQUIRE(res.action == AutoTroubleshootAction::Skipped);
    REQUIRE(res.report_path.empty());

    platform::remove_recursive(run_dir);
}

namespace {

// Mock process runner standing in for the troubleshoot agent. Optionally
// writes `artifact_to_write` (simulating the agent hand-authoring the failed
// node's canonical artifact) and returns a clean exit.
class FakeAgentRunner : public ProcessRunner {
public:
    FakeAgentRunner(std::string artifact_path, std::string content)
        : artifact_path_(std::move(artifact_path)), content_(std::move(content)) {}

    Result<ProcessResult> run(const std::string&, const std::vector<std::string>&,
                              const std::string&, int,
                              const std::map<std::string, std::string>&,
                              const std::string&, int,
                              std::function<void(const std::string&)>) override {
        if (!artifact_path_.empty()) {
            auto slash = artifact_path_.find_last_of("/\\");
            if (slash != std::string::npos) platform::mkdir_p(artifact_path_.substr(0, slash));
            std::ofstream out(artifact_path_);
            out << content_;
        }
        ProcessResult r;
        r.exit_code = 0;
        r.stdout_output = "{}";
        return Result<ProcessResult>::success(std::move(r));
    }

private:
    std::string artifact_path_;
    std::string content_;
};

struct TsFixture {
    std::string base = platform::temp_dir() + "/needle_ts_promote";
    std::string run_dir = base + "/run";
    std::string project_dir = base + "/proj";

    TsFixture() {
        platform::remove_recursive(base);
        platform::mkdir_p(run_dir + "/stages/node");
        platform::mkdir_p(project_dir);
        std::ofstream cp(run_dir + "/checkpoint.json");
        cp << "{\"timestamp\":\"x\",\"current_node\":\"node\",\"completed_nodes\":[],"
              "\"retry_counters\":{},\"context\":{\"needle.last_outcome.status\":\"FAILURE\"},"
              "\"graph_file\":\"\",\"graph_hash\":\"x\"}";
        std::ofstream st(run_dir + "/stages/node/status.json");
        st << "{\"status\":\"FAILURE\",\"timeout_kind\":\"wall_clock\"}";
    }
    ~TsFixture() { platform::remove_recursive(base); }

    Graph graph() {
        AttributeMap node_attrs;
        node_attrs.set("artifact", project_dir + "/out/OUT.md");  // absolute
        Node s{"start", NodeType::START, AttributeMap()};
        Node n{"node", NodeType::CODERGEN, node_attrs};
        Node e{"exit", NodeType::EXIT, AttributeMap()};
        std::vector<Node> nodes = {s, n, e};
        std::vector<Edge> edges = {{"start", "node", AttributeMap()},
                                   {"node", "exit", AttributeMap()}};
        return Graph::make("g", std::move(nodes), std::move(edges));
    }

    Context ctx() {
        Context c;
        c.set("needle.project_dir", project_dir);
        c.set("needle.run_guard_reserved", "true");  // skip RunGuard for the test
        return c;
    }
};

} // anonymous namespace

TEST_CASE("AutoTroubleshoot promotes a hand-authored artifact into engine state",
          "[integration][auto_troubleshoot]") {
    TsFixture fx;
    Graph g = fx.graph();
    Context c = fx.ctx();

    auto runner = std::make_shared<FakeAgentRunner>(fx.project_dir + "/out/OUT.md",
                                                    "RECOVERED STORYBOARD CONTENT");
    AutoTroubleshoot ats(runner);
    auto res = ats.handle("node", g, fx.run_dir, c, 1, TroubleshootMode::Diagnose);

    CHECK(res.action == AutoTroubleshootAction::Promoted);
    CHECK(res.promoted_artifact_output == "RECOVERED STORYBOARD CONTENT");
}

TEST_CASE("AutoTroubleshoot does not promote when no artifact is authored",
          "[integration][auto_troubleshoot]") {
    TsFixture fx;
    Graph g = fx.graph();
    Context c = fx.ctx();

    // Runner writes nothing — the artifact never appears.
    auto runner = std::make_shared<FakeAgentRunner>("", "");
    AutoTroubleshoot ats(runner);
    auto res = ats.handle("node", g, fx.run_dir, c, 1, TroubleshootMode::Diagnose);

    CHECK(res.action != AutoTroubleshootAction::Promoted);
    CHECK(res.promoted_artifact_output.empty());
}
