#include "needle/troubleshoot/allowed_tools.h"

#include <sstream>

namespace needle {

namespace {

std::string quote_if_needed(const std::string& path) {
    if (path.find_first_of(" \t\n\"'") == std::string::npos) return path;
    std::string out = "'";
    for (char c : path) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string write_tool(const std::string& path) {
    return "Write(" + quote_if_needed(path) + ")";
}

std::string edit_tool(const std::string& path) {
    return "Edit(" + quote_if_needed(path) + ")";
}

} // namespace

std::string build_allowed_tools(TroubleshootMode mode,
                                const std::string& project_dir,
                                const std::string& graph_path,
                                const std::string& recovery_dir) {
    // NOTE: Phase 0 spike (sprint-016-allowed-tools-quoting.md) showed
    // that absolute-path patterns are silently ignored by claude. We emit
    // relative-glob patterns; the agent is invoked with cwd=project_dir.
    // The recovery_dir/project_dir arguments are still passed for
    // forward-compat with tier-3 expansions and tests.
    (void)project_dir;
    (void)recovery_dir;
    if (mode == TroubleshootMode::Off) return "";

    std::ostringstream out;
    out << "Read Glob Grep "
        << "Write(recovery.md) "
        << "Write(agent.stdout.log) "
        << "Write(agent.stderr.log) "
        << "Bash(needle troubleshoot escalate:*)";

    if (mode == TroubleshootMode::Tweak || mode == TroubleshootMode::Full) {
        if (!graph_path.empty()) {
            out << " " << edit_tool(graph_path);
        } else {
            out << " " << edit_tool("*.dot");
        }
        out << " "
            << edit_tool(".needle/**/stages/*/prompt.md") << " "
            << "Bash(needle stage mark:*) "
            << "Bash(needle stage advance:*) "
            << "Bash(needle stage retry:*) "
            << "Bash(needle resume:*) "
            << "Bash(needle config set defaults.*:*) "
            << "Bash(git status:*) "
            << "Bash(git log:*) "
            << "Bash(git diff:*)";
    }

    if (mode == TroubleshootMode::Full) {
        out << " "
            << edit_tool("**") << " "
            << write_tool("**") << " "
            << "Bash(git add:*) "
            << "Bash(git checkout:*) "
            << "Bash(git stash:*) "
            << "Bash(npm install:*) "
            << "Bash(npm ci:*) "
            << "Bash(pnpm install:*) "
            << "Bash(yarn install:*) "
            << "Bash(pip install:*) "
            << "Bash(cargo build:*) "
            << "Bash(cargo update:*) "
            << "Bash(make:*)";
    }

    return out.str();
}

} // namespace needle
