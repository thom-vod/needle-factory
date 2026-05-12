#include <catch2/catch.hpp>

#include "needle/backend/cli_backend.h"
#include "needle/backend/process_runner.h"
#include "needle/engine/troubleshoot_agent.h"
#include "needle/platform/platform.h"

#include <fstream>

using namespace needle;

TEST_CASE("TroubleshootAgent accepts valid stage command", "[troubleshoot_agent]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "analysis\nneedle stage retry node";
    mock->enqueue(resp);

    std::map<std::string, CLITemplate> templates;
    templates["claude"] = CLITemplate::claude_default();
    CLIBackend backend(CLITemplate::claude_default(), templates, mock);

    std::string run_dir = platform::temp_dir() + "/needle_ta_test";
    platform::remove_recursive(run_dir);
    platform::mkdir_p(run_dir + "/stages/node");
    std::ofstream st(run_dir + "/stages/node/status.json");
    st << "{}";

    Context ctx;
    ctx.set("needle.project_dir", ".");
    DiagnosisReport report;
    auto out = TroubleshootAgent::run("node", run_dir, ".", report, backend, ctx, 1000);
    REQUIRE(out.ok);
    REQUIRE(out.command.find("needle stage retry node") != std::string::npos);

    platform::remove_recursive(run_dir);
}

TEST_CASE("TroubleshootAgent rejects invalid command", "[troubleshoot_agent]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "needle stage retry other_node";
    mock->enqueue(resp);

    std::map<std::string, CLITemplate> templates;
    templates["claude"] = CLITemplate::claude_default();
    CLIBackend backend(CLITemplate::claude_default(), templates, mock);

    std::string run_dir = platform::temp_dir() + "/needle_ta_test2";
    platform::remove_recursive(run_dir);
    platform::mkdir_p(run_dir + "/stages/node");
    std::ofstream st(run_dir + "/stages/node/status.json");
    st << "{}";

    Context ctx;
    DiagnosisReport report;
    auto out = TroubleshootAgent::run("node", run_dir, ".", report, backend, ctx, 1000);
    REQUIRE_FALSE(out.ok);

    platform::remove_recursive(run_dir);
}
