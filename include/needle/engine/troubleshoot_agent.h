#pragma once

#include <memory>
#include <string>

#include "needle/backend/process_runner.h"
#include "needle/model/context.h"
#include "needle/troubleshoot/diagnose.h"
#include "needle/troubleshoot/types.h"

namespace needle {

struct TroubleshootAgentResult {
    bool ok = false;
    bool timed_out = false;
    int exit_code = -1;
    TroubleshootSessionStatus status = TroubleshootSessionStatus::FailedAgent;
    double cost_usd = 0.0;
    std::string stdout_output;
    std::string stderr_output;
    std::string reasoning;
    std::string error;
};

class TroubleshootAgent {
public:
    static TroubleshootAgentResult run(const std::string& node_id,
                                       const std::string& run_dir,
                                       const std::string& project_dir,
                                       const std::string& graph_path,
                                       const DiagnosisReport& report,
                                       Context& ctx,
                                       TroubleshootMode mode,
                                       std::shared_ptr<ProcessRunner> runner,
                                       int timeout_ms);
};

} // namespace needle
