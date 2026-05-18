#include "needle/troubleshoot/allowed_tools.h"

#include <sstream>

#include "needle/platform/platform.h"

namespace needle {

namespace {

std::string relativise(const std::string& path, const std::string& base) {
    // Empty / non-absolute path: return as-is.
    if (path.empty() || !platform::is_absolute_path(path)) return path;
    if (base.empty() || !platform::is_absolute_path(base)) return path;
    // Normalize trailing slash on base.
    std::string b = base;
    if (b.back() == '/') b.pop_back();
    if (path.size() > b.size() && path.compare(0, b.size(), b) == 0 && path[b.size()] == '/') {
        return path.substr(b.size() + 1);
    }
    return path;  // path is not under base; caller decides what to do
}

std::string quote_if_needed(const std::string& path) {
    // SPRINT-016 M7 fix: include `(` and `)` in the set of characters that
    // trigger quoting — they are claude allow-list grammar metacharacters
    // and an un-quoted operator-controlled path containing them could
    // synthesise an additional Bash(...) allowance.
    if (path.find_first_of(" \t\n\"'()") == std::string::npos) return path;
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
    if (mode == TroubleshootMode::Off) return "";

    std::string session_suffix;  // e.g. ".needle/run-xyz/troubleshoot/session-abc"
    if (!recovery_dir.empty()) {
        session_suffix = relativise(recovery_dir, project_dir);
        if (platform::is_absolute_path(session_suffix)) {
            // session_dir is outside project_dir; use a glob-suffix anchored at the session-<id> leaf
            auto pos = session_suffix.rfind("/session-");
            if (pos != std::string::npos) {
                session_suffix = std::string("**/") + session_suffix.substr(pos + 1);
                // session_suffix now like "**/session-<id>"
            } else {
                session_suffix.clear();
            }
        }
    }

    std::ostringstream out;
    out << "Read Glob Grep ";
    if (!session_suffix.empty()) {
        out << "Write(" << quote_if_needed(session_suffix + "/recovery.md") << ") "
            << "Write(" << quote_if_needed(session_suffix + "/agent.stdout.log") << ") "
            << "Write(" << quote_if_needed(session_suffix + "/agent.stderr.log") << ") ";
    } else {
        // Fallback: bare basenames (matches anywhere ending in these filenames).
        // The pre-N9 behaviour; kept as a last-resort fallback.
        out << "Write(recovery.md) Write(agent.stdout.log) Write(agent.stderr.log) ";
    }
    out << "Bash(needle troubleshoot escalate:*)";

    if (mode == TroubleshootMode::Tweak || mode == TroubleshootMode::Full) {
        std::string graph_rel = relativise(graph_path, project_dir);
        if (graph_rel.empty()) {
            out << " " << edit_tool("*.dot");
        } else if (platform::is_absolute_path(graph_rel)) {
            // Still absolute: outside project_dir. Fall back to basename glob.
            out << " " << edit_tool("*.dot");
        } else {
            out << " " << edit_tool(graph_rel);
        }
        // Claude's Edit(...) path glob is suffix-matched against absolute
        // paths, so this prompt.md allowance is defence-in-depth only. The
        // Phase 4a file-write hook is the authoritative project_dir boundary.
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
