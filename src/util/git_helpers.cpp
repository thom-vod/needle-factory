#include "needle/util/git_helpers.h"

#include <cstdio>
#include <sstream>

namespace needle {
namespace {

Result<std::string> run_capture(const std::string& cmd) {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return Result<std::string>::failure("failed to run: " + cmd);
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), fp)) out += buf;
    int rc = pclose(fp);
    if (rc != 0) return Result<std::string>::failure(out.empty() ? ("command failed: " + cmd) : out);
    return Result<std::string>::success(std::move(out));
}

std::string q(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += (c == '\'') ? "'\\''" : std::string(1, c);
    out += "'";
    return out;
}

} // namespace

Result<std::vector<std::string>> git_log_commits_since(const std::string& repo_dir,
                                                       const std::string& from_commit) {
    auto out = run_capture("git -C " + q(repo_dir) + " log --format=%H " +
                           q(from_commit + "..HEAD") + " --reverse 2>/dev/null");
    if (!out.ok()) return Result<std::vector<std::string>>::failure(out.error());
    std::vector<std::string> commits;
    std::istringstream in(out.value());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) commits.push_back(line);
    }
    return Result<std::vector<std::string>>::success(std::move(commits));
}

Result<void> git_cherry_pick_commit(const std::string& repo_dir,
                                    const std::string& commit_hash) {
    auto out = run_capture("git -C " + q(repo_dir) + " cherry-pick " + q(commit_hash) + " 2>&1");
    if (!out.ok()) return Result<void>::failure(out.error());
    return Result<void>::success();
}

Result<std::string> git_status_porcelain(const std::string& repo_dir) {
    return run_capture("git -C " + q(repo_dir) + " status --porcelain 2>/dev/null");
}

} // namespace needle
