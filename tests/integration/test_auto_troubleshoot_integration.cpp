#include <catch2/catch.hpp>

#include "needle/engine/auto_troubleshoot.h"
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
    auto res = ats.handle("node", g, run_dir, ctx, 1);
    REQUIRE((res.action == AutoTroubleshootAction::Resumed || res.action == AutoTroubleshootAction::Escalated));
    REQUIRE_FALSE(res.report_path.empty());
    REQUIRE(platform::file_exists(res.report_path));

    platform::remove_recursive(run_dir);
}
