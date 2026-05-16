#include "needle/engine/auto_troubleshoot.h"

#include <algorithm>
#include <cstdlib>

#include "needle/config/needle_config.h"
#include "needle/engine/recovery_report.h"
#include "needle/engine/troubleshoot_agent.h"
#include "needle/platform/platform.h"
#include "needle/troubleshoot/diagnose.h"

namespace needle {

namespace {

bool escalation_marker_exists(const std::string& run_dir) {
    if (platform::file_exists(run_dir + "/escalate.json")) return true;
    const std::string root = run_dir + "/troubleshoot";
    if (!platform::is_directory(root)) return false;
    for (const auto& entry : platform::list_directory(root)) {
        if (platform::file_exists(root + "/" + entry + "/escalate.json")) {
            return true;
        }
    }
    return false;
}

} // namespace

AutoTroubleshoot::AutoTroubleshoot(std::shared_ptr<ProcessRunner> runner)
    : runner_(std::move(runner)) {}

AutoTroubleshootResult AutoTroubleshoot::handle(const std::string& node_id,
                                                const Graph& graph,
                                                const std::string& run_dir,
                                                Context& ctx,
                                                int max_attempts_per_stage,
                                                TroubleshootMode mode) {
    (void)graph;
    AutoTroubleshootResult out;
    if (mode == TroubleshootMode::Off) {
        out.action = AutoTroubleshootAction::Skipped;
        out.message = "troubleshoot mode off";
        return out;
    }

    const std::string key = "troubleshoot.attempts." + node_id;
    int prior = 0;
    if (ctx.has(key)) {
        prior = std::atoi(ctx.get(key).c_str());
    }
    if (prior >= max_attempts_per_stage) {
        RecoveryReportInput rep;
        rep.node_id = node_id;
        rep.run_dir = run_dir;
        rep.attempt = prior;
        rep.max_attempts = max_attempts_per_stage;
        rep.action = AutoTroubleshootAction::Escalated;
        rep.result_line = "retry cap reached";
        out.report_path = RecoveryReport::write(rep);
        out.action = AutoTroubleshootAction::Escalated;
        out.message = "retry cap reached";
        return out;
    }
    ctx.set(key, std::to_string(prior + 1));

    DiagnosisReport report = Diagnose::collect_report(run_dir, node_id);

    RecoveryReportInput rep;
    rep.node_id = node_id;
    rep.run_dir = run_dir;
    rep.attempt = prior + 1;
    rep.max_attempts = max_attempts_per_stage;
    rep.diagnosis = report;
    rep.result_line = "agent session placeholder";

    int timeout_ms = 300000;
    auto cfg_to = NeedleConfig::global().get_string("defaults.troubleshoot_agent_timeout");
    if (!cfg_to.empty()) {
        timeout_ms = std::max(1000, std::atoi(cfg_to.c_str()));
    }

    std::string project_dir = ctx.get("needle.project_dir");
    if (project_dir.empty()) project_dir = ".";
    const std::string graph_path = ctx.get("needle.graph_path");

    auto agent = TroubleshootAgent::run(node_id, run_dir, project_dir, graph_path,
                                        report, ctx, mode, runner_, timeout_ms);
    rep.operator_notes = agent.reasoning.empty() ? agent.error : agent.reasoning;
    rep.result_line = "agent exit code " + std::to_string(agent.exit_code);

    if (escalation_marker_exists(run_dir)) {
        rep.action = AutoTroubleshootAction::Escalated;
        out.action = AutoTroubleshootAction::Escalated;
        out.message = "agent escalated";
    } else if (agent.ok && agent.exit_code == 0) {
        rep.action = AutoTroubleshootAction::Resumed;
        out.action = AutoTroubleshootAction::Resumed;
        out.message = "agent session completed";
    } else {
        rep.action = AutoTroubleshootAction::Skipped;
        out.action = AutoTroubleshootAction::Skipped;
        out.message = agent.error.empty() ? "agent session did not complete" : agent.error;
    }

    out.report_path = RecoveryReport::write(rep);
    return out;
}

} // namespace needle
