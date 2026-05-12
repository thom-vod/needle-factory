#include "needle/engine/recovery_report.h"

#include <fstream>

#include "needle/event/event.h"

namespace needle {

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

} // namespace needle
