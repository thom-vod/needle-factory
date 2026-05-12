#include "needle/engine/auto_troubleshoot.h"

#include <sstream>

#include "needle/backend/cli_backend.h"
#include "needle/backend/process_runner.h"
#include "needle/config/needle_config.h"
#include "needle/engine/recovery_report.h"
#include "needle/engine/remediation_executor.h"
#include "needle/engine/remediation_plan.h"
#include "needle/engine/troubleshoot_agent.h"

namespace needle {

namespace {

// Parse the agent's terminal `needle stage *` command into a RemediationPlan
// that the executor can apply. Returns a plan with type=EscalateToOperator
// (and a populated `reason`) when the command is malformed or operates on a
// node the engine can't safely act on.
RemediationPlan parse_agent_command(const std::string& command,
                                    const std::string& failed_node,
                                    const Graph& graph) {
    RemediationPlan p;
    p.node_id = failed_node;
    p.type = RemediationPlan::Type::EscalateToOperator;

    std::istringstream in(command);
    std::vector<std::string> tok;
    std::string t;
    while (in >> t) tok.push_back(t);

    if (tok.size() < 3 || tok[0] != "needle" || tok[1] != "stage") {
        p.reason = "agent command did not start with `needle stage`";
        return p;
    }

    const std::string& verb = tok[2];

    if (verb == "mark") {
        // needle stage mark <node-id> <success|failure> [--output "..."]
        if (tok.size() < 5) {
            p.reason = "mark: missing node-id or status";
            return p;
        }
        if (tok[3] != failed_node) {
            p.reason = "mark must target the failed stage (" + failed_node + ")";
            return p;
        }
        if (tok[4] != "success") {
            p.reason = "agent chose `mark " + tok[4] + "` -> operator decision required";
            return p;
        }
        auto edges = graph.outgoing_edges(failed_node);
        std::string next = edges.empty() ? "" : edges.front()->to;
        p.type = RemediationPlan::Type::MarkSuccessAdvance;
        p.next_node = next;
        p.reason = "agent: mark success + advance";
        return p;
    }

    if (verb == "retry") {
        // needle stage retry <node-id>
        if (tok.size() < 4) {
            p.reason = "retry: missing node-id";
            return p;
        }
        if (tok[3] != failed_node) {
            p.reason = "retry must target the failed stage (" + failed_node + ")";
            return p;
        }
        p.type = RemediationPlan::Type::ResetAndRetry;
        p.reason = "agent: reset and retry";
        return p;
    }

    if (verb == "advance") {
        // needle stage advance --to <node-id>
        std::string target;
        for (size_t i = 3; i + 1 < tok.size(); ++i) {
            if (tok[i] == "--to") { target = tok[i + 1]; break; }
        }
        if (target.empty()) {
            p.reason = "advance: missing --to <node-id>";
            return p;
        }
        if (!graph.find_node(target)) {
            p.reason = "advance target `" + target + "` is not a graph node";
            return p;
        }
        // mark the failed stage as success first, then advance to the target
        p.type = RemediationPlan::Type::MarkSuccessAdvance;
        p.next_node = target;
        p.reason = "agent: mark + advance --to " + target;
        return p;
    }

    p.reason = "unknown stage verb `" + verb + "`";
    return p;
}

} // anonymous namespace

AutoTroubleshoot::AutoTroubleshoot() = default;

AutoTroubleshootResult AutoTroubleshoot::handle(const std::string& node_id,
                                                const Graph& graph,
                                                const std::string& run_dir,
                                                Context& ctx,
                                                int max_attempts_per_stage) {
    AutoTroubleshootResult out;
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

    std::string current_fidelity = "summary:high";
    if (const Node* n = graph.find_node(node_id)) {
        std::string v = n->attrs.get("fidelity");
        if (!v.empty()) current_fidelity = v;
    }

    std::string next_node;
    auto edges = graph.outgoing_edges(node_id);
    if (!edges.empty()) next_node = edges.front()->to;

    RemediationPlan plan = plan_remediation(report, node_id, next_node, current_fidelity);
    std::string last_key = "troubleshoot.last_plan." + node_id;
    std::string plan_name = remediation_type_string(plan.type);
    if (max_attempts_per_stage > 1 && ctx.has(last_key) && ctx.get(last_key) == plan_name) {
        plan.type = RemediationPlan::Type::EscalateToOperator;
        plan.reason = "same remediation cannot be applied twice in a row";
    }
    ctx.set(last_key, plan_name);

    RecoveryReportInput rep;
    rep.node_id = node_id;
    rep.run_dir = run_dir;
    rep.attempt = prior + 1;
    rep.max_attempts = max_attempts_per_stage;
    rep.diagnosis = report;
    rep.plan = plan;

    if (plan.type == RemediationPlan::Type::EscalateToAgent) {
        auto pr = std::make_shared<NativeProcessRunner>();
        std::map<std::string, CLITemplate> templates;
        templates["claude"] = CLITemplate::claude_default();
        templates["codex"] = CLITemplate::codex_default();
        templates["gemini"] = CLITemplate::gemini_default();
        CLIBackend backend(CLITemplate::claude_default(), templates, pr);

        int timeout_ms = 300000;
        auto cfg_to = NeedleConfig::global().get_string("defaults.troubleshoot_agent_timeout");
        if (!cfg_to.empty()) {
            timeout_ms = std::max(1000, std::atoi(cfg_to.c_str()));
        }
        auto agent = TroubleshootAgent::run(node_id, run_dir,
                                            ctx.get("needle.project_dir"), report, backend, ctx,
                                            timeout_ms);
        if (!agent.ok) {
            rep.action = AutoTroubleshootAction::Escalated;
            rep.operator_notes = agent.error;
            rep.result_line = "agent escalation failed";
            out.report_path = RecoveryReport::write(rep);
            out.action = AutoTroubleshootAction::Escalated;
            out.message = agent.error;
            return out;
        }

        // Parse the agent's terminal `needle stage *` command and apply it.
        RemediationPlan agent_plan = parse_agent_command(agent.command, node_id, graph);
        if (agent_plan.type == RemediationPlan::Type::EscalateToOperator) {
            rep.actions_applied.push_back(agent.command);
            rep.operator_notes = agent.reasoning;
            rep.action = AutoTroubleshootAction::Escalated;
            rep.result_line = "agent command rejected: " + agent_plan.reason;
            out.report_path = RecoveryReport::write(rep);
            out.action = AutoTroubleshootAction::Escalated;
            out.message = agent_plan.reason;
            return out;
        }

        Graph agent_graph = graph;
        RemediationExecutionResult agent_exec =
            execute_remediation_plan(agent_plan, agent_graph, run_dir);
        rep.plan = agent_plan;
        rep.actions_applied = agent_exec.actions;
        rep.operator_notes = agent.reasoning;
        if (agent_exec.resumed && !agent_exec.escalated) {
            rep.action = AutoTroubleshootAction::Resumed;
            rep.result_line = "agent decision applied: " + agent.command;
            out.report_path = RecoveryReport::write(rep);
            out.action = AutoTroubleshootAction::Resumed;
            out.message = "agent resumed via " + remediation_type_string(agent_plan.type);
            return out;
        }
        rep.action = AutoTroubleshootAction::Escalated;
        rep.result_line = agent_exec.message.empty()
            ? "agent decision could not be applied"
            : agent_exec.message;
        out.report_path = RecoveryReport::write(rep);
        out.action = AutoTroubleshootAction::Escalated;
        out.message = agent_exec.message;
        return out;
    }

    if (plan.type == RemediationPlan::Type::EscalateToOperator) {
        rep.action = AutoTroubleshootAction::Escalated;
        rep.result_line = "operator escalation";
        out.report_path = RecoveryReport::write(rep);
        out.action = AutoTroubleshootAction::Escalated;
        if (report.kind == FailureKind::CherryPickConflict) {
            out.message = "fan-in cherry-pick conflict; merge manually in launch repo";
        } else {
            out.message = "operator escalation";
        }
        return out;
    }

    Graph mutable_graph = graph;
    RemediationExecutionResult exec = execute_remediation_plan(plan, mutable_graph, run_dir);
    rep.actions_applied = exec.actions;
    rep.result_line = exec.message;

    if (exec.resumed && !exec.escalated) {
        rep.action = AutoTroubleshootAction::Resumed;
        out.report_path = RecoveryReport::write(rep);
        out.action = AutoTroubleshootAction::Resumed;
        out.message = "resumed";
        return out;
    }

    rep.action = AutoTroubleshootAction::Escalated;
    out.report_path = RecoveryReport::write(rep);
    out.action = AutoTroubleshootAction::Escalated;
    out.message = exec.message;
    return out;
}

} // namespace needle
