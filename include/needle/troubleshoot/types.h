#pragma once

#include <string>

#include "needle/model/maybe.h"

namespace needle {

enum class TroubleshootMode {
    Off,
    Diagnose,
    Tweak,
    Full,
};

enum class TroubleshootSessionStatus {
    Running,
    Resumed,
    Reported,
    Escalated,
    Cancelled,
    FailedTimeout,
    FailedAgent,
};

std::string to_string(TroubleshootMode m);
Maybe<TroubleshootMode> parse_troubleshoot_mode(const std::string& s);
Maybe<TroubleshootMode> parse_troubleshoot_mode_graph_attr(const std::string& s);

std::string to_string(TroubleshootSessionStatus s);
Maybe<TroubleshootSessionStatus> parse_troubleshoot_session_status(const std::string& s);

} // namespace needle
