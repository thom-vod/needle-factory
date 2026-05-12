#include "needle/engine/fan_in_merger.h"

#include <algorithm>
#include <sstream>

#include "needle/util/git_helpers.h"
#include "needle/worktree/manager.h"

namespace needle {
namespace {

std::vector<std::string> parse_conflicting_files(const std::string& porcelain) {
    std::vector<std::string> out;
    std::istringstream in(porcelain);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 4) continue;
        char x = line[0];
        char y = line[1];
        if ((x == 'U' || y == 'U') || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) {
            out.push_back(line.substr(3));
        }
    }
    return out;
}

} // namespace

Result<FanInMergeResult> FanInMerger::merge(const std::string& launch_repo,
                                            const std::string& launch_commit,
                                            const std::vector<std::string>& branch_ids,
                                            const Context& ctx,
                                            const WorktreeConfig& worktree_cfg) {
    std::vector<std::string> ordered = branch_ids;
    std::sort(ordered.begin(), ordered.end());

    std::vector<std::string> applied;
    for (size_t i = 0; i < ordered.size(); ++i) {
        const std::string& branch_id = ordered[i];
        std::string wt = ctx.get("needle.branch_worktree." + branch_id);
        if (wt.empty()) continue;

        auto commits = git_log_commits_since(wt, launch_commit);
        if (!commits.ok()) return Result<FanInMergeResult>::failure(commits.error());
        for (const auto& hash : commits.value()) {
            auto cp = git_cherry_pick_commit(launch_repo, hash);
            if (!cp.ok()) {
                auto st = git_status_porcelain(launch_repo);
                FanInMergeResult out;
                out.ok = false;
                out.conflict.branch_that_conflicted = branch_id;
                out.conflict.branches_already_applied = applied;
                out.conflict.branches_pending.assign(ordered.begin() + static_cast<long>(i + 1), ordered.end());
                out.conflict.git_status = st.ok() ? st.value() : "";
                out.conflict.conflicting_files = parse_conflicting_files(out.conflict.git_status);
                return Result<FanInMergeResult>::success(std::move(out));
            }
        }
        applied.push_back(branch_id);
    }

    if (worktree_cfg.cleanup == "remove-on-success") {
        for (const auto& branch_id : ordered) {
            std::string wt = ctx.get("needle.branch_worktree." + branch_id);
            if (wt.empty()) continue;
            (void)WorktreeManager::remove(wt);
        }
    }
    FanInMergeResult ok;
    ok.ok = true;
    return Result<FanInMergeResult>::success(std::move(ok));
}

} // namespace needle
