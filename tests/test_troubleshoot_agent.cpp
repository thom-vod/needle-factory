#include <catch2/catch.hpp>

#include "needle/backend/process_runner.h"
#include "needle/engine/troubleshoot_agent.h"
#include "needle/platform/platform.h"

#include <algorithm>
#include <fstream>

using namespace needle;

TEST_CASE("TroubleshootAgent invokes claude with tweak allow-list", "[troubleshoot_agent]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = R"({"type":"result","subtype":"success","is_error":false,"result":"done","total_cost_usd":0.25})";
    mock->enqueue(resp);

    std::string run_dir = platform::temp_dir() + "/needle_ta_test";
    platform::remove_recursive(run_dir);
    platform::mkdir_p(run_dir + "/stages/node");
    std::ofstream st(run_dir + "/stages/node/status.json");
    st << "{}";

    Context ctx;
    ctx.set("needle.project_dir", ".");
    DiagnosisReport report;
    auto out = TroubleshootAgent::run("node", run_dir, run_dir + "/troubleshoot/session-test",
                                      ".", "/tmp/graph.dot", report,
                                      ctx, TroubleshootMode::Tweak, mock, 1000);
    REQUIRE(out.ok);
    REQUIRE(out.exit_code == 0);
    REQUIRE(out.cost_usd == Approx(0.25));
    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].command == "claude");
    REQUIRE(calls[0].working_dir == ".");
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(), "--verbose") != calls[0].args.end());
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(), "--allowed-tools") != calls[0].args.end());

    platform::remove_recursive(run_dir);
}

TEST_CASE("TroubleshootAgent full mode skips permissions", "[troubleshoot_agent]") {
    auto mock = std::make_shared<MockProcessRunner>();
    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = R"({"type":"result","subtype":"success","is_error":false,"result":"done"})";
    mock->enqueue(resp);

    std::string run_dir = platform::temp_dir() + "/needle_ta_test2";
    platform::remove_recursive(run_dir);
    platform::mkdir_p(run_dir + "/stages/node");
    std::ofstream st(run_dir + "/stages/node/status.json");
    st << "{}";

    Context ctx;
    DiagnosisReport report;
    auto out = TroubleshootAgent::run("node", run_dir, run_dir + "/troubleshoot/session-test",
                                      ".", "/tmp/graph.dot", report,
                                      ctx, TroubleshootMode::Full, mock, 1000);
    REQUIRE(out.ok);
    auto calls = mock->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(),
                      "--dangerously-skip-permissions") != calls[0].args.end());
    REQUIRE(std::find(calls[0].args.begin(), calls[0].args.end(),
                      "--allowed-tools") == calls[0].args.end());

    platform::remove_recursive(run_dir);
}
