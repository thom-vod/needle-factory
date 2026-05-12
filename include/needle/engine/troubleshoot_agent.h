#pragma once

#include <string>

#include "needle/backend/backend.h"
#include "needle/backend/cli_backend.h"
#include "needle/troubleshoot/diagnose.h"

namespace needle {

struct TroubleshootAgentResult {
    bool ok = false;
    bool timed_out = false;
    std::string command;
    std::string reasoning;
    std::string error;
};

class TroubleshootAgent {
public:
    static TroubleshootAgentResult run(const std::string& node_id,
                                       const std::string& run_dir,
                                       const std::string& project_dir,
                                       const DiagnosisReport& report,
                                       CLIBackend& backend,
                                       Context& ctx,
                                       int timeout_ms);
};

} // namespace needle
