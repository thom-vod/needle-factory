#pragma once

#include <string>

#include "needle/troubleshoot/types.h"

namespace needle {

std::string build_allowed_tools(TroubleshootMode mode,
                                const std::string& project_dir,
                                const std::string& graph_path,
                                const std::string& recovery_dir);

} // namespace needle
