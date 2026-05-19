#include "needle/troubleshoot/allowed_tools.h"

#include <sstream>

namespace needle {

namespace {

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

std::string edit_tool(const std::string& path) {
    return "Edit(" + quote_if_needed(path) + ")";
}

// Empirical finding (claude 2.1.144, /opt/homebrew/bin/claude, verified
// 2026-05-18 via direct probe under /tmp/needle_perm_probe_*): ANY
// parenthesised Write(...) pattern is denied at the permission-prompt
// layer — Write(**), Write(**/*), Write(<basename>), and
// Write(<absolute path>) all fail to match the invoked file_path. Only
// bare `Write` (no parens) is accepted. The Edit(...) and Bash(...)
// parenthesised forms continue to match suffix-globs as documented.
//
// Consequence: we emit `Write` bare in the base allow-list and let the
// SPRINT-017 Phase 4a file-write hook be the authoritative project_dir
// ∪ session_dir boundary. The hook canonicalises each Edit/Write
// tool_use's file_path post-run and flips the session outcome to
// failed_hook_violation on any escape. See troubleshooter-design.md
// §Security model.
constexpr const char* kBareWrite = "Write";

} // namespace

std::string build_allowed_tools(TroubleshootMode mode,
                                const std::string& project_dir,
                                const std::string& graph_path,
                                const std::string& recovery_dir) {
    // NOTE: Phase 0 spike (sprint-016-allowed-tools-quoting.md) showed
    // that absolute-path patterns are silently ignored by claude. We emit
    // relative-glob patterns; the agent is invoked with cwd=project_dir.
    if (mode == TroubleshootMode::Off) return "";

    // recovery_dir is retained for signature compatibility; once Claude's
    // Write(...) matcher supports path globs the per-session-scoped
    // patterns can be reinstated. For now Write is bare and the file-write
    // hook does the scoping.
    (void)project_dir;
    (void)graph_path;
    (void)recovery_dir;

    std::ostringstream out;
    out << "Read Glob Grep " << kBareWrite << " ";
    out << "Bash(needle troubleshoot escalate:*)";

    if (mode == TroubleshootMode::Tweak || mode == TroubleshootMode::Full) {
        out << " " << edit_tool(".needle/**/source.dot")
            << " " << edit_tool("*.dot");
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
        // Write is already bare in the base allow-list. Full adds the
        // broader Edit(**) glob; the file-write hook is the boundary.
        out << " "
            << edit_tool("**") << " "
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
