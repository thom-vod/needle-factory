#pragma once

#include <string>
#include <vector>

#include "needle/engine/remediation_plan.h"
#include "needle/model/graph.h"

namespace needle {

struct RemediationExecutionResult {
    bool resumed = false;
    bool escalated = false;
    std::string message;
    std::vector<std::string> actions;
};

RemediationExecutionResult execute_remediation_plan(const RemediationPlan& plan,
                                                    Graph& graph,
                                                    const std::string& run_dir);

} // namespace needle
