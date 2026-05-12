#pragma once

#include <string>
#include <vector>

#include "needle/model/context.h"
#include "needle/model/result.h"
#include "needle/worktree/strategy.h"

namespace needle {

struct CherryPickConflict {
    std::string branch_that_conflicted;
    std::vector<std::string> branches_already_applied;
    std::vector<std::string> branches_pending;
    std::vector<std::string> conflicting_files;
    std::string git_status;
};

struct FanInMergeResult {
    bool ok = true;
    CherryPickConflict conflict;
};

class FanInMerger {
public:
    static Result<FanInMergeResult> merge(const std::string& launch_repo,
                                          const std::string& launch_commit,
                                          const std::vector<std::string>& branch_ids,
                                          const Context& ctx,
                                          const WorktreeConfig& worktree_cfg);
};

} // namespace needle
