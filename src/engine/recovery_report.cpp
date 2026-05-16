#include "needle/engine/recovery_report.h"

#include <iomanip>
#include <fstream>
#include <sstream>

#include "needle/event/event.h"

namespace needle {

namespace {

std::string yaml_string(const std::string& value) {
    if (value.empty()) return "\"\"";
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n' || c == '\r') {
            out += ' ';
        } else {
            out.push_back(c);
        }
    }
    out += "\"";
    return out;
}

std::string yaml_nullable_string(const std::string& value) {
    if (value.empty()) return "null";
    return yaml_string(value);
}

std::string money(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

} // namespace

std::string RecoveryReport::write(const RecoveryReportInput& input) {
    std::string ts = utc_timestamp_now();
    std::string safe = ts;
    for (char& c : safe) {
        if (c == ':') c = '-';
    }
    std::string out_path = input.run_dir + "/recovery-" + safe + ".md";

    std::ofstream out(out_path);
    if (!out.is_open()) return "";

    out << "# Auto-Troubleshoot Recovery Report — " << input.node_id << "\n\n";
    out << "**Timestamp:** " << ts << "\n";
    out << "**Stage:** " << input.node_id << "\n";
    out << "**Attempt:** " << input.attempt << " of " << input.max_attempts << "\n\n";

    out << "## Classifier Output\n\n";
    out << Diagnose::render_markdown(input.diagnosis) << "\n\n";

    out << "## Remediation\n\n";
    out << "- **Plan:** " << remediation_type_string(input.plan.type) << "\n";
    out << "- **Rationale:** " << input.plan.reason << "\n";
    out << "- **Actions applied:**\n";
    for (const auto& a : input.actions_applied) {
        out << "  - `" << a << "`\n";
    }

    out << "\n## Result\n\n";
    out << "- **Action:** "
        << (input.action == AutoTroubleshootAction::Resumed ? "Resumed" :
            input.action == AutoTroubleshootAction::Escalated ? "Escalated" : "Skipped")
        << "\n";
    out << "- **Detail:** " << input.result_line << "\n\n";

    out << "## Operator Notes\n\n";
    out << input.operator_notes << "\n";

    return out_path;
}

std::string RecoveryReport::write_v2(const RecoveryReportV2Input& input,
                                     const std::string& output_path) {
    std::ofstream out(output_path);
    if (!out.is_open()) return "";

    out << "---\n";
    out << "schema_version: 2\n";
    out << "session_id: " << yaml_string(input.session_id) << "\n";
    out << "run_id: " << yaml_string(input.run_id) << "\n";
    out << "tier: " << to_string(input.mode) << "\n";
    out << "trust: " << to_string(input.trust) << "\n";
    out << "agent: " << yaml_string(input.agent) << "\n";
    out << "model: " << yaml_string(input.model) << "\n";
    out << "started: " << yaml_string(input.started) << "\n";
    out << "ended: " << yaml_string(input.ended) << "\n";
    out << "cost_usd: " << money(input.cost_usd) << "\n";
    out << "budget_usd: " << money(input.budget_usd) << "\n";
    out << "outcome: " << to_string(input.outcome) << "\n";
    out << "failed_node: " << yaml_string(input.failed_node) << "\n";
    out << "attempts_used: " << input.attempts_used << "\n";
    out << "escalate_reason: " << yaml_nullable_string(input.escalate_reason) << "\n";
    out << "---\n\n";

    out << "## Diagnosis\n\n";
    if (input.diagnosis_body.empty()) {
        out << "_No diagnosis recorded._\n\n";
    } else {
        out << input.diagnosis_body;
        if (input.diagnosis_body.empty() || input.diagnosis_body.back() != '\n') out << "\n";
        out << "\n";
    }

    out << "## Actions taken\n\n";
    if (input.action_log.empty()) {
        out << "- No actions recorded.\n\n";
    } else {
        for (const auto& action : input.action_log) {
            out << "- " << action << "\n";
        }
        out << "\n";
    }

    out << "## Outcome\n\n";
    if (input.outcome_summary.empty()) {
        out << to_string(input.outcome) << "\n\n";
    } else {
        out << input.outcome_summary << "\n\n";
    }

    out << "## Artifacts\n\n";
    if (input.artifacts.empty()) {
        out << "- None\n";
    } else {
        for (const auto& artifact : input.artifacts) {
            out << "- " << artifact << "\n";
        }
    }

    return output_path;
}

} // namespace needle
