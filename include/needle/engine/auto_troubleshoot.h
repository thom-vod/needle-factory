#pragma once

#include <string>

#include "needle/model/context.h"
#include "needle/model/graph.h"

namespace needle {

enum class AutoTroubleshootAction {
    Skipped,
    Resumed,
    Escalated,
};

struct AutoTroubleshootResult {
    AutoTroubleshootAction action = AutoTroubleshootAction::Skipped;
    std::string report_path;
    std::string message;
};

class AutoTroubleshoot {
public:
    AutoTroubleshoot();

    AutoTroubleshootResult handle(const std::string& node_id,
                                  const Graph& graph,
                                  const std::string& run_dir,
                                  Context& ctx,
                                  int max_attempts_per_stage);
};

} // namespace needle
