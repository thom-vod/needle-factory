#pragma once

#include <string>
#include <vector>

#include "needle/engine/auto_troubleshoot.h"
#include "needle/engine/remediation_plan.h"
#include "needle/troubleshoot/diagnose.h"

namespace needle {

struct RecoveryReportInput {
    std::string node_id;
    std::string run_dir;
    int attempt = 0;
    int max_attempts = 0;
    DiagnosisReport diagnosis;
    RemediationPlan plan;
    AutoTroubleshootAction action = AutoTroubleshootAction::Skipped;
    std::vector<std::string> actions_applied;
    std::string operator_notes;
    std::string result_line;
};

class RecoveryReport {
public:
    static std::string write(const RecoveryReportInput& input);
};

} // namespace needle
