#pragma once

#include <string>

#include "needle/model/result.h"
#include "needle/troubleshoot/types.h"

namespace needle {

class TroubleshootSnapshot {
public:
    static Result<void> capture(const std::string& project_dir,
                                const std::string& graph_path,
                                const std::string& session_dir,
                                TroubleshootMode mode);

    static Result<void> restore(const std::string& project_dir,
                                const std::string& session_dir);
};

} // namespace needle
