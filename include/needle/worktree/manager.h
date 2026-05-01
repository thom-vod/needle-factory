#pragma once

#include <string>
#include "needle/model/result.h"
#include "needle/worktree/strategy.h"

namespace needle {

struct WorktreeReadyInfo {
    std::string path;        // absolute path of the worktree
    std::string branch;      // branch name
    bool created_now;        // true if we just created the worktree, false if pre-existing
};

// Wraps `git worktree add` / list / remove. Best-effort: shells out to git
// and parses output. All operations idempotent under repeated invocations.
class WorktreeManager {
public:
    // Resolve the worktree at `cfg.path` against `launch_repo`. If the
    // worktree exists and is on `cfg.branch`: continue silently. If not
    // present: `git worktree add <path> -b <branch>`. If present on a
    // different branch: fail with a clear error (no auto-switch).
    static Result<WorktreeReadyInfo> ensure_ready(const std::string& launch_repo,
                                                  const WorktreeConfig& cfg);

    // Remove an active worktree (used by cleanup=remove-on-success in v2).
    static Result<void> remove(const std::string& path);

    // Best-effort check: is the directory inside a needle-managed worktree?
    static bool is_active_worktree(const std::string& path);
};

} // namespace needle
