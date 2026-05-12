#include "needle/engine/remediation_plan.h"

namespace needle {

std::string remediation_type_string(RemediationPlan::Type type) {
    switch (type) {
        case RemediationPlan::Type::MarkSuccessAdvance: return "MarkSuccessAdvance";
        case RemediationPlan::Type::ResetAndRetry: return "ResetAndRetry";
        case RemediationPlan::Type::ResetWithLowerFidelity: return "ResetWithLowerFidelity";
        case RemediationPlan::Type::KillOrphansFirst: return "KillOrphansFirst";
        case RemediationPlan::Type::EscalateToAgent: return "EscalateToAgent";
        case RemediationPlan::Type::EscalateToOperator: return "EscalateToOperator";
    }
    return "EscalateToOperator";
}

static std::string next_lower_fidelity(const std::string& current_fidelity) {
    if (current_fidelity == "summary:high") return "summary:medium";
    if (current_fidelity == "summary:medium") return "summary:low";
    if (current_fidelity == "summary:low") return "summary:low";
    if (current_fidelity == "full") return "summary:high";
    return "summary:medium";
}

RemediationPlan plan_remediation(const DiagnosisReport& report,
                                 const std::string& node_id,
                                 const std::string& next_node,
                                 const std::string& current_fidelity) {
    RemediationPlan plan;
    plan.node_id = node_id;
    plan.next_node = next_node;
    plan.source_kind = report.kind;

    switch (report.kind) {
        case FailureKind::WallClockWithProgress:
        case FailureKind::IdleStallWorkCommitted:
            plan.type = RemediationPlan::Type::MarkSuccessAdvance;
            plan.reason = "failure kind indicates progress is salvageable";
            break;
        case FailureKind::WallClockWithoutOwnProgress:
            plan.type = RemediationPlan::Type::ResetAndRetry;
            plan.reason = "wall-clock without own progress gets one retry";
            break;
        case FailureKind::IdleStallNoWorkSalvageable:
        case FailureKind::PromptBlowup:
            plan.type = RemediationPlan::Type::ResetWithLowerFidelity;
            plan.fidelity_override = next_lower_fidelity(current_fidelity);
            plan.reason = "retry with lower prompt fidelity";
            break;
        case FailureKind::IdleStallWorkOnDisk:
        case FailureKind::SelfExitError:
            plan.type = RemediationPlan::Type::EscalateToAgent;
            plan.reason = "requires constrained agent judgment";
            break;
        case FailureKind::RolePromptConflict:
        case FailureKind::VariableCorrupted:
        case FailureKind::CherryPickConflict:
        case FailureKind::Unknown:
            plan.type = RemediationPlan::Type::EscalateToOperator;
            plan.reason = "requires operator intervention";
            break;
        case FailureKind::OrphanedSubprocesses:
            plan.type = RemediationPlan::Type::KillOrphansFirst;
            plan.primary_after_orphan = RemediationPlan::Type::EscalateToOperator;
            plan.reason = "kill orphaned subprocesses first";
            break;
    }

    if (!report.signals.descendant_pids.empty() && plan.type != RemediationPlan::Type::KillOrphansFirst) {
        plan.primary_after_orphan = plan.type;
        plan.type = RemediationPlan::Type::KillOrphansFirst;
        plan.orphan_pids = report.signals.descendant_pids;
    }

    return plan;
}

} // namespace needle
