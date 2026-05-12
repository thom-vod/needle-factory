#pragma once

#include <string>
#include <vector>

#include "needle/model/result.h"

namespace needle {

Result<std::vector<std::string>> git_log_commits_since(const std::string& repo_dir,
                                                       const std::string& from_commit);

Result<void> git_cherry_pick_commit(const std::string& repo_dir,
                                    const std::string& commit_hash);

Result<std::string> git_status_porcelain(const std::string& repo_dir);

} // namespace needle
