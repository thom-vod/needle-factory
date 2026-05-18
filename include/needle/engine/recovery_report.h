#pragma once

#include <string>
#include <vector>

#include "needle/engine/auto_troubleshoot.h"
#include "needle/troubleshoot/types.h"

namespace needle {

struct RecoveryReportV2Input {
    std::string session_id;
    std::string run_id;
    std::string failed_node;
    TroubleshootMode mode = TroubleshootMode::Tweak;
    std::string agent;
    std::string model;
    std::string started;
    std::string ended;
    double cost_usd = 0.0;
    TroubleshootSessionStatus outcome = TroubleshootSessionStatus::FailedAgent;
    int attempts_used = 0;
    std::string escalate_reason;
    std::string backup_branch;
    std::string backup_base;
    std::string diagnosis_body;
    std::vector<std::string> action_log;
    std::string outcome_summary;
    std::vector<std::string> artifacts;
};

class RecoveryReport {
public:
    static std::string write_v2(const RecoveryReportV2Input& input,
                                const std::string& output_path);
};

} // namespace needle
