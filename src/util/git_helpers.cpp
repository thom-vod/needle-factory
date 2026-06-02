#include "needle/util/git_helpers.h"
#include "needle/backend/process_runner.h"

#include <sstream>
#include <vector>

namespace needle {
namespace {

// Run `git -C <repo_dir> <args...>` via the process runner (no shell). The
// previous popen + single-quoted shell string failed under cmd.exe on Windows
// (the single quotes were passed literally to git and `/dev/null` is not a
// valid path), so these helpers were completely non-functional there. Output
// includes stderr to preserve the old `2>&1` error reporting.
struct GitResult {
    std::string output;
    int exit_code = -1;
    bool launched = false;
};

GitResult run_git(const std::string& repo_dir, const std::vector<std::string>& args) {
    std::vector<std::string> full;
    full.reserve(args.size() + 2);
    full.push_back("-C");
    full.push_back(repo_dir);
    for (const auto& a : args) full.push_back(a);

    NativeProcessRunner runner;
    auto r = runner.run("git", full, repo_dir, 30000);
    GitResult out;
    if (!r.ok()) return out;  // launch failure; exit_code stays -1
    out.launched = true;
    out.output = r.value().stdout_output + r.value().stderr_output;
    out.exit_code = r.value().exit_code;
    return out;
}

} // namespace

Result<std::vector<std::string>> git_log_commits_since(const std::string& repo_dir,
                                                       const std::string& from_commit) {
    auto out = run_git(repo_dir,
                       {"log", "--format=%H", from_commit + "..HEAD", "--reverse"});
    if (!out.launched || out.exit_code != 0) {
        return Result<std::vector<std::string>>::failure(
            out.output.empty() ? "git log failed" : out.output);
    }
    std::vector<std::string> commits;
    std::istringstream in(out.output);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) commits.push_back(line);
    }
    return Result<std::vector<std::string>>::success(std::move(commits));
}

Result<void> git_cherry_pick_commit(const std::string& repo_dir,
                                    const std::string& commit_hash) {
    auto out = run_git(repo_dir, {"cherry-pick", commit_hash});
    if (!out.launched || out.exit_code != 0) {
        return Result<void>::failure(out.output.empty() ? "git cherry-pick failed" : out.output);
    }
    return Result<void>::success();
}

Result<std::string> git_status_porcelain(const std::string& repo_dir) {
    auto out = run_git(repo_dir, {"status", "--porcelain"});
    if (!out.launched || out.exit_code != 0) {
        return Result<std::string>::failure(out.output.empty() ? "git status failed" : out.output);
    }
    return Result<std::string>::success(std::move(out.output));
}

} // namespace needle
