#include "needle/engine/auto_troubleshoot.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <thread>

#include "needle/config/needle_config.h"
#include "needle/event/event.h"
#include "needle/engine/recovery_report.h"
#include "needle/engine/run_guard.h"
#include "needle/engine/troubleshoot_agent.h"
#include "needle/engine/troubleshoot_backup.h"
#include "needle/platform/platform.h"
#include "needle/troubleshoot/diagnose.h"
#include "needle/troubleshoot/stream_parser.h"
#include "needle/troubleshoot/write_hook.h"
#include "needle/util/logger.h"
#include "needle/util/timestamp.h"

namespace needle {

namespace {

bool escalation_marker_exists(const std::string& session_dir) {
    return platform::file_exists(session_dir + "/escalate.json");
}

bool cancel_marker_exists(const std::string& session_dir) {
    return platform::file_exists(session_dir + "/cancel.json");
}

struct EscalationMarker {
    std::string reason;
    std::string next_question;
    std::string last_summary;
};

bool read_escalation_marker(const std::string& session_dir, EscalationMarker& marker) {
    std::ifstream in(session_dir + "/escalate.json");
    if (!in.is_open()) return false;
    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_object()) return false;
        if (j.contains("reason") && j["reason"].is_string()) {
            marker.reason = j["reason"].get<std::string>();
        }
        if (j.contains("escalate_reason") && j["escalate_reason"].is_string()) {
            marker.reason = j["escalate_reason"].get<std::string>();
        }
        if (j.contains("next_question") && j["next_question"].is_string()) {
            marker.next_question = j["next_question"].get<std::string>();
        }
        if (j.contains("last_summary") && j["last_summary"].is_string()) {
            marker.last_summary = j["last_summary"].get<std::string>();
        }
    } catch (const std::exception&) {
        return false;
    }
    return !marker.reason.empty();
}

std::string file_tail(const std::string& path, size_t max_chars) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string value = ss.str();
    if (value.size() <= max_chars) return value;
    return value.substr(value.size() - max_chars);
}

std::string escalation_opener(const EscalationMarker& marker,
                              const std::string& run_id,
                              const std::string& node_id) {
    std::ostringstream opener;
    opener << "Troubleshooter escalated.\n\n"
           << "Run: " << run_id << "\n"
           << "Failed node: " << node_id << "\n\n"
           << "Reason: " << marker.reason;
    if (!marker.next_question.empty()) {
        opener << "\n\nNext question: " << marker.next_question;
    }
    if (!marker.last_summary.empty()) {
        // Wrap in a fenced block; strip triple-backtick runs first so the
        // tail can't terminate the block early.
        std::string sanitised = marker.last_summary;
        size_t pos = std::string::npos;
        while ((pos = sanitised.find("```")) != std::string::npos) {
            sanitised.replace(pos, 3, "'''");
        }
        opener << "\n\nAgent summary:\n```\n" << sanitised << "\n```";
    }
    return opener.str();
}

std::string basename_of(const std::string& path) {
    size_t end = path.find_last_not_of("/\\");
    if (end == std::string::npos) return path;
    size_t start = path.find_last_of("/\\", end);
    const size_t begin = start == std::string::npos ? 0 : start + 1;
    return path.substr(begin, end - begin + 1);
}

bool mode_mutates_project(TroubleshootMode mode) {
    return mode == TroubleshootMode::Tweak || mode == TroubleshootMode::Full;
}

std::string absolute_path_for_manifest(const std::string& path) {
    if (path.empty() || platform::is_absolute_path(path)) return path;
    return platform::path_join(platform::getcwd_str(), path);
}

void write_write_hook_manifest(const std::string& session_dir,
                               const std::string& project_dir) {
    nlohmann::json manifest;
    manifest["schema_version"] = 1;
    manifest["project_dir"] = absolute_path_for_manifest(project_dir);
    manifest["session_dir"] = absolute_path_for_manifest(session_dir);
    manifest["policy"] = "deny_outside";

    std::ofstream out(session_dir + "/write-hook.json");
    if (!out.is_open()) {
        NEEDLE_LOG_WARN("troubleshoot", "failed to write write-hook manifest for %s",
                        session_dir.c_str());
        return;
    }
    out << manifest.dump(2) << "\n";
}

void touch_file(const std::string& path) {
    std::ofstream out(path, std::ios::app);
}

std::string create_session_dir(const std::string& run_dir, std::string& session_id,
                               const std::string& requested_session_id = "") {
    session_id = requested_session_id.empty() ? utc_timestamp_now_dashes() : requested_session_id;
    std::string session_dir = run_dir + "/troubleshoot/session-" + session_id;
    while (requested_session_id.empty() &&
           (platform::file_exists(session_dir) || platform::is_directory(session_dir))) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        session_id = utc_timestamp_now_dashes();
        session_dir = run_dir + "/troubleshoot/session-" + session_id;
    }
    platform::mkdir_p(session_dir);
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

} // namespace

void process_agent_stream_line(const std::string& line,
                               TroubleshootStreamParser& parser,
                               std::ofstream& events_out,
                               EventBus* event_bus,
                               const std::string& run_id,
                               const std::string& session_id,
                               const std::string& node_id,
                               TroubleshootMode mode) {
    if (line.empty()) return;
    std::vector<TroubleshootStreamEvent> events = parser.parse_line(line);
    if (events_out.is_open()) {
        events_out << line << "\n";
    }
    for (const auto& parsed : events) {
        nlohmann::json payload = parsed.payload;
        payload["failed_node"] = node_id;
        payload["mode"] = to_string(mode);
        emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                                   parsed.type, std::move(payload));
    }
}

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
        std::string session_dir = create_session_dir(
            run_dir, session_id, ctx.get("needle.troubleshoot_session_id"));
        out.session_id = session_id;
        const std::string started = utc_timestamp_now();
        RecoveryReportV2Input rep;
        rep.session_id = session_id;
        rep.run_id = ctx.get("needle.run_id");
        if (rep.run_id.empty()) rep.run_id = basename_of(run_dir);
        rep.failed_node = node_id;
        rep.mode = mode;
        rep.agent = "claude";
        rep.model = "claude-opus-4-7";
        // (retry-cap-reached path: no config lookup needed; this run never
        // touched the agent binary.)
        rep.started = started;
        rep.ended = utc_timestamp_now();
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
    std::string session_dir = create_session_dir(
        run_dir, session_id, ctx.get("needle.troubleshoot_session_id"));
    out.session_id = session_id;
    const std::string started = utc_timestamp_now();
    std::string run_id = ctx.get("needle.run_id");
    if (run_id.empty()) run_id = basename_of(run_dir);

    std::optional<GuardReleaser> guard;
    if (ctx.get("needle.run_guard_reserved") != "true") {
        guard = RunGuard::try_reserve(run_id);
        if (!guard) {
            out.action = AutoTroubleshootAction::Skipped;
            out.message = "another troubleshoot session is in flight for run " + run_id;
            return out;
        }
    }

    int timeout_ms = 300000;
    auto cfg_to = NeedleConfig::global().get_string("defaults.troubleshoot_agent_timeout");
    if (!cfg_to.empty()) {
        timeout_ms = std::max(1000, std::atoi(cfg_to.c_str()));
    }

    // SPRINT-016 M6 fix: defaults.troubleshoot_agent / _model are
    // honoured. Default values used in TroubleshootAgent::run mirror
    // these so the report and the invocation agree on what ran.
    std::string agent_name = NeedleConfig::global().get_string("defaults.troubleshoot_agent");
    if (agent_name.empty()) agent_name = "claude";
    std::string agent_model = NeedleConfig::global().get_string("defaults.troubleshoot_model");
    if (agent_model.empty()) agent_model = "claude-opus-4-7";

    std::string project_dir = ctx.get("needle.project_dir");
    if (project_dir.empty()) project_dir = ".";
    const std::string graph_path = ctx.get("needle.graph_path");
    write_write_hook_manifest(session_dir, project_dir);

    // Backup-branch capture for any tier that mutates project state.
    // Phase 2 wires the real capture; Phase 1 leaves these empty.
    BackupInfo backup;
    if (mode_mutates_project(mode)) {
        auto captured = TroubleshootBackup::capture(project_dir, run_id, session_id, session_dir);
        if (!captured.ok()) {
            out.action = AutoTroubleshootAction::Skipped;
            out.message = captured.error();
            return out;
        }
        backup = captured.value();
    }

    nlohmann::json started_payload;
    started_payload["failed_node"] = node_id;
    started_payload["mode"] = to_string(mode);
    if (!backup.branch.empty()) {
        started_payload["backup_branch"] = backup.branch;
        started_payload["backup_base"] = backup.base_sha;
    }
    emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                               "session_started", std::move(started_payload));

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
                                      run_id, session_id, node_id, mode);
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
                                  run_id, session_id, node_id, mode);
        stream_buffer.clear();
    }
    events_out.flush();
    events_out.close();
    {
        std::ofstream stdout_log(session_dir + "/agent.stdout.log");
        stdout_log << agent.stdout_output;
    }
    {
        std::ofstream stderr_log(session_dir + "/agent.stderr.log");
        stderr_log << agent.stderr_output;
    }
    if (!backup.base_sha.empty()) {
        auto touched = TroubleshootBackup::record_agent_touch(
            project_dir, backup.base_sha, session_dir);
        if (!touched.ok()) {
            NEEDLE_LOG_WARN("troubleshoot", "failed to record agent modified files: %s",
                            touched.error().c_str());
        }
    }
    TroubleshootSessionStatus outcome = agent.status;
    std::string escalate_reason;
    EscalationMarker escalation;
    bool parsed_escalation = false;
    bool was_cancelled = cancel_marker_exists(session_dir);
    if (escalation_marker_exists(session_dir)) {
        if (read_escalation_marker(session_dir, escalation)) {
            parsed_escalation = true;
            outcome = TroubleshootSessionStatus::Escalated;
            escalate_reason = escalation.reason;
            out.action = AutoTroubleshootAction::Escalated;
            out.message = escalate_reason;
            escalation.last_summary = file_tail(session_dir + "/agent.stdout.log", 500);

            const std::string synthetic_node_id = "troubleshoot-escalate-" + session_id;
            // Fresh session per escalation (M9 fix); previously stomped the run's
            // shared InteractiveSession.
            auto session = std::make_shared<InteractiveSession>();
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->node_id = synthetic_node_id;
                session->prompt = "Troubleshooter escalation for " + node_id;
                session->context_summary = escalation.last_summary;
                session->pipeline_context = "Run: " + run_id + "\nFailed node: " + node_id + "\n";
                session->previous_node_id.clear();
                session->active = true;
                session->continued = false;
                session->go_back = false;
                session->final_result.clear();
                session->opener = escalation_opener(escalation, run_id, node_id);
            }
            InteractiveSessionRegistry::register_session(synthetic_node_id, session);

            nlohmann::json escalated_payload;
            escalated_payload["reason"] = escalation.reason;
            escalated_payload["next_question"] = escalation.next_question;
            escalated_payload["last_summary"] = escalation.last_summary;
            escalated_payload["interactive_node_id"] = synthetic_node_id;
            escalated_payload["failed_node"] = node_id;
            escalated_payload["mode"] = to_string(mode);
            emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                                       "session_escalated", std::move(escalated_payload));
        }
    }
    if (parsed_escalation) {
        // already set above
    } else if (was_cancelled) {
        outcome = TroubleshootSessionStatus::Cancelled;
        out.action = AutoTroubleshootAction::Cancelled;
        out.message = "session cancelled";
    } else if (mode == TroubleshootMode::Diagnose &&
               agent.ok && agent.exit_code == 0) {
        // Diagnose is read-only — never trigger a pipeline retry.
        outcome = TroubleshootSessionStatus::Reported;
        out.action = AutoTroubleshootAction::Reported;
        out.message = "diagnose report written";
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
        out.action = AutoTroubleshootAction::Skipped;
        out.message = agent.error.empty() ? "agent session did not complete" : agent.error;
    }

    std::vector<HookViolation> hook_violations;
    hook_violations = audit_events_ndjson(session_dir + "/events.ndjson",
                                         project_dir, session_dir);
    if (!hook_violations.empty()) {
        outcome = TroubleshootSessionStatus::FailedHookViolation;
        out.action = AutoTroubleshootAction::Skipped;
        out.message = "file write hook violation";
    }

    RecoveryReportV2Input rep2;
    rep2.session_id = session_id;
    rep2.run_id = run_id;
    rep2.failed_node = node_id;
    rep2.mode = mode;
    rep2.agent = agent_name;
    rep2.model = agent_model;
    rep2.started = started;
    rep2.ended = utc_timestamp_now();
    rep2.cost_usd = agent.cost_usd;
    rep2.outcome = outcome;
    rep2.attempts_used = prior + 1;
    rep2.escalate_reason = escalate_reason;
    rep2.backup_branch = backup.branch;
    rep2.backup_base = backup.base_sha;
    rep2.diagnosis_body = Diagnose::render_markdown(report);
    rep2.action_log.push_back("agent exit code " + std::to_string(agent.exit_code));
    if (!agent.reasoning.empty()) rep2.action_log.push_back(agent.reasoning);
    rep2.outcome_summary = outcome == TroubleshootSessionStatus::Escalated
        ? "Escalated: " + escalate_reason
        : status_summary(outcome, node_id, out.message);
    for (const auto& violation : hook_violations) {
        rep2.security_audit_lines.push_back(
            violation.tool + " " + violation.file_path +
            " (tool_use_id=" + violation.tool_use_id + ")");
    }
    rep2.artifacts = {
        "events: " + session_dir + "/events.ndjson",
        "agent_stdout: " + session_dir + "/agent.stdout.log",
        "agent_stderr: " + session_dir + "/agent.stderr.log"
    };
    if (!backup.branch.empty()) {
        rep2.artifacts.push_back("backup_branch: " + backup.branch);
        rep2.artifacts.push_back("rollback: needle troubleshoot rollback " +
                                 run_dir + " " + session_id);
    }
    out.report_path = RecoveryReport::write_v2(rep2, session_dir + "/recovery.md");
    nlohmann::json report_payload;
    report_payload["report_path"] = out.report_path;
    report_payload["failed_node"] = node_id;
    report_payload["mode"] = to_string(mode);
    emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                               "report_written", std::move(report_payload));
    nlohmann::json completed_payload;
    completed_payload["outcome"] = to_string(outcome);
    completed_payload["summary"] = status_summary(outcome, node_id, out.message);
    completed_payload["failed_node"] = node_id;
    completed_payload["mode"] = to_string(mode);
    emit_troubleshoot_activity(event_bus, run_id, session_id, node_id,
                               "session_completed", std::move(completed_payload));
    return out;
}

} // namespace needle
