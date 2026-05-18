#include <catch2/catch.hpp>

#include "needle/engine/recovery_report.h"
#include "needle/platform/platform.h"

#include <fstream>
#include <sstream>

using namespace needle;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

RecoveryReportV2Input base_input() {
    RecoveryReportV2Input input;
    input.session_id = "2026-05-12T05-32-44Z";
    input.run_id = "run-1";
    input.failed_node = "node";
    input.mode = TroubleshootMode::Tweak;
    input.agent = "claude";
    input.model = "claude-opus-4-7";
    input.started = "2026-05-12T05:32:44Z";
    input.ended = "2026-05-12T05:33:44Z";
    input.attempts_used = 1;
    input.diagnosis_body = "- **Failure kind:** self_exit_error";
    input.action_log = {"read status.json", "needle retry node"};
    input.outcome_summary = "resumed at 05:33:44";
    input.artifacts = {"events: troubleshoot/session/events.ndjson"};
    return input;
}

std::string write_report(const RecoveryReportV2Input& input, const std::string& name) {
    std::string dir = platform::temp_dir() + "/needle_recovery_report_v2";
    platform::mkdir_p(dir);
    std::string path = dir + "/" + name + ".md";
    REQUIRE(RecoveryReport::write_v2(input, path) == path);
    return read_file(path);
}

} // namespace

TEST_CASE("RecoveryReport v2 writes resumed report with cost", "[recovery_report]") {
    auto input = base_input();
    input.outcome = TroubleshootSessionStatus::Resumed;
    input.cost_usd = 0.43;

    std::string body = write_report(input, "resumed");
    REQUIRE(body.find("schema_version: 2") != std::string::npos);
    REQUIRE(body.find("session_id: \"2026-05-12T05-32-44Z\"") != std::string::npos);
    REQUIRE(body.find("tier: tweak") != std::string::npos);
    REQUIRE(body.find("cost_usd: 0.43") != std::string::npos);
    REQUIRE(body.find("outcome: resumed") != std::string::npos);
    REQUIRE(body.find("## Diagnosis") != std::string::npos);
    REQUIRE(body.find("## Actions taken") != std::string::npos);
    REQUIRE(body.find("- needle retry node") != std::string::npos);
}

TEST_CASE("RecoveryReport v2 writes escalation reason", "[recovery_report]") {
    auto input = base_input();
    input.outcome = TroubleshootSessionStatus::Escalated;
    input.escalate_reason = "parallel race needs operator";
    input.outcome_summary = "escalated to operator";

    std::string body = write_report(input, "escalated");
    REQUIRE(body.find("outcome: escalated") != std::string::npos);
    REQUIRE(body.find("escalate_reason: \"parallel race needs operator\"") != std::string::npos);
    REQUIRE(body.find("escalated to operator") != std::string::npos);
}

TEST_CASE("RecoveryReport v2 writes timeout outcome", "[recovery_report]") {
    auto input = base_input();
    input.outcome = TroubleshootSessionStatus::FailedTimeout;
    input.cost_usd = 1.25;
    input.outcome_summary = "failed after timeout";

    std::string body = write_report(input, "timeout");
    REQUIRE(body.find("outcome: failed_timeout") != std::string::npos);
    REQUIRE(body.find("cost_usd: 1.25") != std::string::npos);
}

TEST_CASE("RecoveryReport v2 handles empty action log", "[recovery_report]") {
    auto input = base_input();
    input.outcome = TroubleshootSessionStatus::FailedAgent;
    input.action_log.clear();

    std::string body = write_report(input, "empty_actions");
    REQUIRE(body.find("## Actions taken") != std::string::npos);
    REQUIRE(body.find("- No actions recorded.") != std::string::npos);
}
