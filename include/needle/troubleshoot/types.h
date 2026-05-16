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

enum class TroubleshootTrust {
    Snapshot,
    WorktreeBranch,
};

enum class TroubleshootSessionStatus {
    Running,
    Resumed,
    Escalated,
    FailedKilledBudget,
    FailedTimeout,
    FailedAgent,
};

std::string to_string(TroubleshootMode m);
Maybe<TroubleshootMode> parse_troubleshoot_mode(const std::string& s);

std::string to_string(TroubleshootTrust t);
Maybe<TroubleshootTrust> parse_troubleshoot_trust(const std::string& s);

std::string to_string(TroubleshootSessionStatus s);

} // namespace needle
