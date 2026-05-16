#include "needle/troubleshoot/types.h"

#include <algorithm>
#include <cctype>

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

std::string to_string(TroubleshootTrust t) {
    switch (t) {
    case TroubleshootTrust::Snapshot:
        return "snapshot";
    case TroubleshootTrust::WorktreeBranch:
        return "worktree_branch";
    }
    return "";
}

Maybe<TroubleshootTrust> parse_troubleshoot_trust(const std::string& s) {
    const std::string v = normalized(s);
    if (v == "snapshot") return Maybe<TroubleshootTrust>(TroubleshootTrust::Snapshot);
    if (v == "worktree_branch") return Maybe<TroubleshootTrust>(TroubleshootTrust::WorktreeBranch);
    return Maybe<TroubleshootTrust>();
}

std::string to_string(TroubleshootSessionStatus s) {
    switch (s) {
    case TroubleshootSessionStatus::Running:
        return "running";
    case TroubleshootSessionStatus::Resumed:
        return "resumed";
    case TroubleshootSessionStatus::Escalated:
        return "escalated";
    case TroubleshootSessionStatus::FailedKilledBudget:
        return "failed_killed_budget";
    case TroubleshootSessionStatus::FailedTimeout:
        return "failed_timeout";
    case TroubleshootSessionStatus::FailedAgent:
        return "failed_agent";
    }
    return "";
}

} // namespace needle
