#pragma once

#include <string>
#include <map>
#include "needle/model/result.h"

namespace needle {

// Per-project worktree strategy. See ~/notes/needle-worktree-proposal.md.
enum class WorktreeStrategy {
    Off,      // legacy behaviour (no worktree). Default.
    Auto,     // needle creates worktree from templates at run-start.
    Manual,   // operator created worktree by hand; needle records it. (v2)
};

WorktreeStrategy worktree_strategy_from_string(const std::string& s);
std::string to_string(WorktreeStrategy strategy);

// Resolved worktree configuration for a single run. Computed once at
// run-start by combining project config + DOT graph attrs + run-flags.
struct WorktreeConfig {
    WorktreeStrategy strategy = WorktreeStrategy::Off;
    std::string branch;        // resolved (after template interpolation)
    std::string path;          // absolute path
    std::string cleanup;       // "keep" | "prompt" | "remove-on-success"
};

// Resolution helpers: interpolate `${run_id}`, `${repo_basename}`, `${pbi_id}`,
// etc. against a parameter map. Returns failure if a referenced variable
// is missing.
Result<std::string> interpolate_template(const std::string& tmpl,
                                         const std::map<std::string, std::string>& params);

class TroubleshootWorktree {
public:
    static Result<std::string> create(const std::string& project_dir,
                                      const std::string& run_id,
                                      const std::string& session_dir);
    static Result<void> apply(const std::string& project_dir,
                              const std::string& run_id);
    static Result<void> discard(const std::string& project_dir,
                                const std::string& run_id);
};

} // namespace needle
