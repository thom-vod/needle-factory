#pragma once

#include <memory>
#include <string>

#include "needle/backend/process_runner.h"
#include "needle/model/context.h"
#include "needle/model/graph.h"
#include "needle/troubleshoot/types.h"

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
    explicit AutoTroubleshoot(std::shared_ptr<ProcessRunner> runner = nullptr);

    AutoTroubleshootResult handle(const std::string& node_id,
                                  const Graph& graph,
                                  const std::string& run_dir,
                                  Context& ctx,
                                  int max_attempts_per_stage,
                                  TroubleshootMode mode);

private:
    std::shared_ptr<ProcessRunner> runner_;
};

} // namespace needle
