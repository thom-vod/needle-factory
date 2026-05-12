#pragma once

#include <string>
#include <vector>

#include "needle/troubleshoot/diagnose.h"

namespace needle {

struct RemediationPlan {
    enum class Type {
        MarkSuccessAdvance,
        ResetAndRetry,
        ResetWithLowerFidelity,
        KillOrphansFirst,
        EscalateToAgent,
        EscalateToOperator,
    };

    Type type = Type::EscalateToOperator;
    std::string node_id;
    std::string next_node;
    std::string fidelity_override;
    std::vector<int> orphan_pids;
    std::string reason;
    FailureKind source_kind = FailureKind::Unknown;
    Type primary_after_orphan = Type::EscalateToOperator;
};

RemediationPlan plan_remediation(const DiagnosisReport& report,
                                 const std::string& node_id,
                                 const std::string& next_node,
                                 const std::string& current_fidelity);

std::string remediation_type_string(RemediationPlan::Type type);

} // namespace needle
