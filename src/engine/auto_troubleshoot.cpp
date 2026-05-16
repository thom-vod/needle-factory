#include "needle/engine/auto_troubleshoot.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#include "needle/config/needle_config.h"
#include "needle/event/event.h"
#include "needle/engine/recovery_report.h"
#include "needle/engine/troubleshoot_agent.h"
#include "needle/platform/platform.h"
#include "needle/troubleshoot/diagnose.h"
#include "needle/troubleshoot/stream_parser.h"
#include "needle/util/timestamp.h"

namespace needle {

namespace {

bool escalation_marker_exists(const std::string& session_dir) {
    return platform::file_exists(session_dir + "/escalate.json");
}

std::string read_escalate_reason(const std::string& session_dir) {
    std::ifstream in(session_dir + "/escalate.json");
    if (!in.is_open()) return "";
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("reason") && j["reason"].is_string()) {
            return j["reason"].get<std::string>();
        }
        if (j.contains("escalate_reason") && j["escalate_reason"].is_string()) {
            return j["escalate_reason"].get<std::string>();
        }
    } catch (const std::exception&) {
    }
    return "";
}

std::string basename_of(const std::string& path) {
    size_t end = path.find_last_not_of("/\\");
    if (end == std::string::npos) return path;
    size_t start = path.find_last_of("/\\", end);
    const size_t begin = start == std::string::npos ? 0 : start + 1;
    return path.substr(begin, end - begin + 1);
}

double budget_for_mode(TroubleshootMode mode) {
    switch (mode) {
    case TroubleshootMode::Diagnose:
        return 0.20;
    case TroubleshootMode::Tweak:
        return 1.00;
    case TroubleshootMode::Full:
        return 5.00;
    case TroubleshootMode::Off:
        return 0.0;
    }
    return 0.0;
}

TroubleshootTrust trust_for_mode(TroubleshootMode mode) {
    return mode == TroubleshootMode::Full
        ? TroubleshootTrust::WorktreeBranch
        : TroubleshootTrust::Snapshot;
}

void touch_file(const std::string& path) {
    std::ofstream out(path, std::ios::app);
}

std::string create_session_dir(const std::string& run_dir, std::string& session_id) {
    session_id = utc_timestamp_now_dashes();
    std::string session_dir = run_dir + "/troubleshoot/session-" + session_id;
    while (platform::file_exists(session_dir) || platform::is_directory(session_dir)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        session_id = utc_timestamp_now_dashes();
        session_dir = run_dir + "/troubleshoot/session-" + session_id;
    }
    platform::mkdir_p(session_dir + "/snapshot");
    touch_file(session_dir + "/events.ndjson");
    touch_file(session_dir + "/agent.stdout.log");
    touch_file(session_dir + "/agent.stderr.log");
    return session_dir;
}

std::string status_summary(TroubleshootSessionStatus status,
                           const std::string& node_id,
                           const std::string& detail) {
    std::string summary = to_string(status);
    if (!node_id.empty()) summary += " for " + node_id;
    if (!detail.empty()) summary += ": " + detail;
    return summary;
}

void emit_troubleshoot_activity(EventBus* bus,
                                const std::string& run_id,
                                const std::string& session_id,
                                const std::string& node_id,
                                const std::string& event_type,
                                nlohmann::json payload) {
    if (!bus) return;
    payload["run_id"] = run_id;
    payload["session_id"] = session_id;
    PipelineEvent e;
    e.type = EventType::TROUBLESHOOT_ACTIVITY;
    e.timestamp = utc_timestamp_now();
    e.node_id = node_id;
    e.message = "troubleshoot " + event_type;
    e.data["run_id"] = run_id;
    e.data["session_id"] = session_id;
    e.data["event_type"] = event_type;
    e.data["payload"] = std::move(payload);
    bus->emit(e);
}

void process_agent_stream_line(const std::string& line,
                               TroubleshootStreamParser& parser,
                               std::ofstream& events_out,
                               EventBus* event_bus,
                               const std::string& run_id,
                               const std::string& session_id,
                               const std::string& node_id,
                               TroubleshootMode mode,
                               TroubleshootTrust trust,
                               double budget_usd) {
    if (line.empty()) return;
    std::vector<TroubleshootStreamEvent> events = parser.parse_line(line);
    for (const auto& parsed : events) {
        if (events_out.is_open()) {
            events_out << parsed.raw_line << "\n";
        }
        nlohmann::json payload = parsed.payload;
        payload["failed_node"] = node_id;
        payload["mode"] = to_string(mode);
        payload["trust"] = to_string(trust);
        payload["budget_usd"] = budget_usd;
        if (parsed.type == "cost_update" &&
            payload.contains("cost_usd") && payload["cost_usd"].is_number() &&
            budget_usd > 0.0) {
            payload["fraction"] = payload["cost_usd"].get<double>() / budget_usd;
        }
        emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                                   parsed.type, std::move(payload));
    }
}

} // namespace

AutoTroubleshoot::AutoTroubleshoot(std::shared_ptr<ProcessRunner> runner)
    : runner_(std::move(runner)) {}

AutoTroubleshootResult AutoTroubleshoot::handle(const std::string& node_id,
                                                const Graph& graph,
                                                const std::string& run_dir,
                                                Context& ctx,
                                                int max_attempts_per_stage,
                                                TroubleshootMode mode,
                                                EventBus* event_bus) {
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
        std::string session_id;
        std::string session_dir = create_session_dir(run_dir, session_id);
        out.session_id = session_id;
        const std::string started = utc_timestamp_now();
        RecoveryReportV2Input rep;
        rep.session_id = session_id;
        rep.run_id = ctx.get("needle.run_id");
        if (rep.run_id.empty()) rep.run_id = basename_of(run_dir);
        rep.failed_node = node_id;
        rep.mode = mode;
        rep.trust = trust_for_mode(mode);
        rep.agent = "claude";
        rep.model = "claude-opus-4-7";
        rep.started = started;
        rep.ended = utc_timestamp_now();
        rep.budget_usd = budget_for_mode(mode);
        rep.outcome = TroubleshootSessionStatus::Escalated;
        rep.attempts_used = prior;
        rep.escalate_reason = "retry cap reached";
        rep.diagnosis_body = "Retry cap reached before launching an agent session.";
        rep.outcome_summary = status_summary(rep.outcome, node_id, rep.escalate_reason);
        rep.artifacts = {
            "events: " + session_dir + "/events.ndjson",
            "agent_stdout: " + session_dir + "/agent.stdout.log",
            "agent_stderr: " + session_dir + "/agent.stderr.log"
        };
        out.report_path = RecoveryReport::write_v2(rep, session_dir + "/recovery.md");
        out.action = AutoTroubleshootAction::Escalated;
        out.message = "retry cap reached";
        return out;
    }
    ctx.set(key, std::to_string(prior + 1));

    DiagnosisReport report = Diagnose::collect_report(run_dir, node_id);
    std::string session_id;
    std::string session_dir = create_session_dir(run_dir, session_id);
    out.session_id = session_id;
    const std::string started = utc_timestamp_now();
    std::string run_id = ctx.get("needle.run_id");
    if (run_id.empty()) run_id = basename_of(run_dir);

    int timeout_ms = 300000;
    auto cfg_to = NeedleConfig::global().get_string("defaults.troubleshoot_agent_timeout");
    if (!cfg_to.empty()) {
        timeout_ms = std::max(1000, std::atoi(cfg_to.c_str()));
    }

    std::string project_dir = ctx.get("needle.project_dir");
    if (project_dir.empty()) project_dir = ".";
    const std::string graph_path = ctx.get("needle.graph_path");

    TroubleshootStreamParser stream_parser;
    std::ofstream events_out(session_dir + "/events.ndjson", std::ios::app);
    std::string stream_buffer;
    auto stdout_callback = [&](const std::string& chunk) {
        stream_buffer += chunk;
        size_t pos = std::string::npos;
        while ((pos = stream_buffer.find('\n')) != std::string::npos) {
            std::string line = stream_buffer.substr(0, pos);
            if (!line.empty() && line[line.size() - 1] == '\r') line.pop_back();
            process_agent_stream_line(line, stream_parser, events_out, event_bus,
                                      run_id, session_id, node_id, mode,
                                      trust_for_mode(mode), budget_for_mode(mode));
            stream_buffer.erase(0, pos + 1);
        }
    };

    auto agent = TroubleshootAgent::run(node_id, run_dir, session_dir, project_dir, graph_path,
                                        report, ctx, mode, runner_, timeout_ms,
                                        stdout_callback);
    if (!stream_buffer.empty()) {
        if (!stream_buffer.empty() && stream_buffer[stream_buffer.size() - 1] == '\r') {
            stream_buffer.pop_back();
        }
        process_agent_stream_line(stream_buffer, stream_parser, events_out, event_bus,
                                  run_id, session_id, node_id, mode,
                                  trust_for_mode(mode), budget_for_mode(mode));
        stream_buffer.clear();
    }
    {
        std::ofstream stdout_log(session_dir + "/agent.stdout.log");
        stdout_log << agent.stdout_output;
    }
    {
        std::ofstream stderr_log(session_dir + "/agent.stderr.log");
        stderr_log << agent.stderr_output;
    }
    TroubleshootSessionStatus outcome = agent.status;
    std::string escalate_reason;
    if (escalation_marker_exists(session_dir)) {
        outcome = TroubleshootSessionStatus::Escalated;
        escalate_reason = read_escalate_reason(session_dir);
        if (escalate_reason.empty()) escalate_reason = "agent escalated";
        out.action = AutoTroubleshootAction::Escalated;
        out.message = "agent escalated";
        nlohmann::json escalated_payload;
        escalated_payload["reason"] = escalate_reason;
        escalated_payload["failed_node"] = node_id;
        escalated_payload["mode"] = to_string(mode);
        escalated_payload["trust"] = to_string(trust_for_mode(mode));
        escalated_payload["budget_usd"] = budget_for_mode(mode);
        emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                                   "session_escalated", std::move(escalated_payload));
    } else if (agent.ok && agent.exit_code == 0 &&
               outcome == TroubleshootSessionStatus::Resumed) {
        outcome = TroubleshootSessionStatus::Resumed;
        out.action = AutoTroubleshootAction::Resumed;
        out.message = "agent session completed";
    } else {
        if (outcome == TroubleshootSessionStatus::Running ||
            outcome == TroubleshootSessionStatus::Resumed) {
            outcome = agent.timed_out
                ? TroubleshootSessionStatus::FailedTimeout
                : TroubleshootSessionStatus::FailedAgent;
        }
        if (agent.cost_usd > budget_for_mode(mode) && budget_for_mode(mode) > 0.0) {
            outcome = TroubleshootSessionStatus::FailedKilledBudget;
        }
        out.action = AutoTroubleshootAction::Skipped;
        out.message = agent.error.empty() ? "agent session did not complete" : agent.error;
    }

    RecoveryReportV2Input rep2;
    rep2.session_id = session_id;
    rep2.run_id = run_id;
    rep2.failed_node = node_id;
    rep2.mode = mode;
    rep2.trust = trust_for_mode(mode);
    rep2.agent = "claude";
    rep2.model = "claude-opus-4-7";
    rep2.started = started;
    rep2.ended = utc_timestamp_now();
    rep2.cost_usd = agent.cost_usd;
    rep2.budget_usd = budget_for_mode(mode);
    rep2.outcome = outcome;
    rep2.attempts_used = prior + 1;
    rep2.escalate_reason = escalate_reason;
    rep2.diagnosis_body = Diagnose::render_markdown(report);
    rep2.action_log.push_back("agent exit code " + std::to_string(agent.exit_code));
    if (!agent.reasoning.empty()) rep2.action_log.push_back(agent.reasoning);
    rep2.outcome_summary = status_summary(outcome, node_id, out.message);
    rep2.artifacts = {
        "events: " + session_dir + "/events.ndjson",
        "snapshot: " + session_dir + "/snapshot",
        "agent_stdout: " + session_dir + "/agent.stdout.log",
        "agent_stderr: " + session_dir + "/agent.stderr.log"
    };
    out.report_path = RecoveryReport::write_v2(rep2, session_dir + "/recovery.md");
    nlohmann::json report_payload;
    report_payload["report_path"] = out.report_path;
    report_payload["failed_node"] = node_id;
    report_payload["mode"] = to_string(mode);
    report_payload["trust"] = to_string(trust_for_mode(mode));
    report_payload["budget_usd"] = budget_for_mode(mode);
    emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                               "report_written", std::move(report_payload));
    return out;
}

} // namespace needle
