#include "needle/troubleshoot/types.h"

#include <algorithm>
#include <cctype>

#include "needle/util/logger.h"

namespace needle {

namespace {

std::string normalized(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

} // namespace

std::string to_string(TroubleshootMode m) {
    switch (m) {
    case TroubleshootMode::Off:
        return "off";
    case TroubleshootMode::Diagnose:
        return "diagnose";
    case TroubleshootMode::Tweak:
        return "tweak";
    case TroubleshootMode::Full:
        return "full";
    }
    return "";
}

Maybe<TroubleshootMode> parse_troubleshoot_mode(const std::string& s) {
    const std::string v = normalized(s);
    if (v == "off" || v == "false") return Maybe<TroubleshootMode>(TroubleshootMode::Off);
    if (v == "diagnose") return Maybe<TroubleshootMode>(TroubleshootMode::Diagnose);
    if (v == "tweak" || v == "true") return Maybe<TroubleshootMode>(TroubleshootMode::Tweak);
    if (v == "full") return Maybe<TroubleshootMode>(TroubleshootMode::Full);
    return Maybe<TroubleshootMode>();
}

Maybe<TroubleshootMode> parse_troubleshoot_mode_graph_attr(const std::string& s) {
    const std::string v = normalized(s);
    thread_local bool warned_true = false;
    thread_local bool warned_false = false;

    if (v == "true" && !warned_true) {
        warned_true = true;
        NEEDLE_LOG_WARN("troubleshoot",
                        "[deprecated] troubleshoot_on_failure=\"true\" is deprecated; use troubleshoot_on_failure=\"tweak\" instead. See the SPRINT-016 release notes.");
    } else if (v == "false" && !warned_false) {
        warned_false = true;
        NEEDLE_LOG_WARN("troubleshoot",
                        "[deprecated] troubleshoot_on_failure=\"false\" is deprecated; use troubleshoot_on_failure=\"off\" instead. See the SPRINT-016 release notes.");
    }

    return parse_troubleshoot_mode(s);
}

std::string to_string(TroubleshootSessionStatus s) {
    switch (s) {
    case TroubleshootSessionStatus::Running:
        return "running";
    case TroubleshootSessionStatus::Resumed:
        return "resumed";
    case TroubleshootSessionStatus::Reported:
        return "reported";
    case TroubleshootSessionStatus::Escalated:
        return "escalated";
    case TroubleshootSessionStatus::Cancelled:
        return "cancelled";
    case TroubleshootSessionStatus::FailedKilledBudget:
        return "failed_killed_budget";
    case TroubleshootSessionStatus::FailedTimeout:
        return "failed_timeout";
    case TroubleshootSessionStatus::FailedAgent:
        return "failed_agent";
    }
    return "";
}

Maybe<TroubleshootSessionStatus> parse_troubleshoot_session_status(const std::string& s) {
    const std::string v = normalized(s);
    if (v == "running") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::Running);
    if (v == "resumed") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::Resumed);
    if (v == "reported") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::Reported);
    if (v == "escalated") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::Escalated);
    if (v == "cancelled") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::Cancelled);
    if (v == "failed_killed_budget") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::FailedKilledBudget);
    if (v == "failed_timeout") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::FailedTimeout);
    if (v == "failed_agent") return Maybe<TroubleshootSessionStatus>(TroubleshootSessionStatus::FailedAgent);
    return Maybe<TroubleshootSessionStatus>();
}

} // namespace needle
