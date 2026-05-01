#include "needle/worktree/manager.h"

#include "needle/util/logger.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define popen _popen
#define pclose _pclose
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <sys/wait.h>
#endif

namespace needle {

namespace {

// Run `git <args>` in `cwd`, capture stdout. Returns {output, exit_code}.
struct GitOut {
    std::string stdout_text;
    int exit_code = -1;
};

GitOut run_git(const std::string& cwd, const std::string& args) {
    GitOut out;
    std::string cmd = "cd '";
    for (char c : cwd) {
        if (c == '\'') cmd += "'\\''";
        else cmd += c;
    }
    cmd += "' && git " + args + " 2>&1";

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return out;

    char buf[4096];
    while (true) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) break;
        out.stdout_text.append(buf, n);
    }
    int rc = pclose(fp);
#ifdef _WIN32
    // _pclose returns the spawned process's exit code directly.
    out.exit_code = rc;
#else
    if (WIFEXITED(rc)) {
        out.exit_code = WEXITSTATUS(rc);
    }
#endif
    return out;
}

bool dir_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Parse `git worktree list --porcelain` output into a {path -> branch} map.
// Each entry block has lines like "worktree <path>", "branch refs/heads/<name>".
std::map<std::string, std::string> parse_worktree_list(const std::string& text) {
    std::map<std::string, std::string> out;
    std::stringstream ss(text);
    std::string line;
    std::string current_path;
    while (std::getline(ss, line)) {
        if (line.rfind("worktree ", 0) == 0) {
            current_path = line.substr(9);
            out[current_path] = "";  // detached or unknown branch by default
        } else if (line.rfind("branch ", 0) == 0 && !current_path.empty()) {
            std::string ref = line.substr(7);
            const std::string prefix = "refs/heads/";
            if (ref.rfind(prefix, 0) == 0) ref = ref.substr(prefix.size());
            out[current_path] = ref;
        } else if (line.empty()) {
            current_path.clear();
        }
    }
    return out;
}

} // anonymous

Result<WorktreeReadyInfo> WorktreeManager::ensure_ready(
        const std::string& launch_repo, const WorktreeConfig& cfg) {
    if (cfg.path.empty() || cfg.branch.empty()) {
        return Result<WorktreeReadyInfo>::failure(
            "WorktreeConfig requires non-empty path and branch");
    }

    // Verify launch_repo is a git repo (and not bare).
    GitOut bare_check = run_git(launch_repo, "rev-parse --is-bare-repository");
    if (bare_check.exit_code != 0) {
        return Result<WorktreeReadyInfo>::failure(
            "launch directory is not a git repo: " + launch_repo);
    }
    std::string bare_trim = bare_check.stdout_text;
    while (!bare_trim.empty() && (bare_trim.back() == '\n' || bare_trim.back() == '\r')) {
        bare_trim.pop_back();
    }
    if (bare_trim == "true") {
        return Result<WorktreeReadyInfo>::failure(
            "cannot create worktree from a bare repo: " + launch_repo);
    }

    WorktreeReadyInfo info;
    info.path = cfg.path;
    info.branch = cfg.branch;

    // Existing worktree? Compare by realpath since macOS /tmp resolves
    // through /private and `git worktree list` reports the canonical path.
    GitOut list = run_git(launch_repo, "worktree list --porcelain");
    auto existing = parse_worktree_list(list.stdout_text);

    auto canonicalize = [](const std::string& p) -> std::string {
        char buf[PATH_MAX];
#ifdef _WIN32
        if (GetFullPathNameA(p.c_str(), PATH_MAX, buf, nullptr)) return std::string(buf);
#else
        if (::realpath(p.c_str(), buf) != nullptr) return std::string(buf);
#endif
        return p;
    };
    std::string canonical_target = canonicalize(cfg.path);

    for (const auto& kv : existing) {
        if (canonicalize(kv.first) == canonical_target) {
            if (kv.second != cfg.branch) {
                return Result<WorktreeReadyInfo>::failure(
                    "worktree at " + cfg.path + " is on branch '" + kv.second +
                    "' but expected branch '" + cfg.branch +
                    "' — refusing to switch automatically");
            }
            info.created_now = false;
            NEEDLE_LOG_INFO("worktree", "worktree already ready at %s on branch %s",
                            cfg.path.c_str(), cfg.branch.c_str());
            return Result<WorktreeReadyInfo>::success(info);
        }
    }

    if (dir_exists(cfg.path)) {
        return Result<WorktreeReadyInfo>::failure(
            "directory already exists at " + cfg.path +
            " but is not a registered git worktree — refusing to overwrite");
    }

    // Create.
    std::string add_cmd = "worktree add ";
    // Quote the path and branch.
    std::string q_path = "'";
    for (char c : cfg.path) { if (c == '\'') q_path += "'\\''"; else q_path += c; }
    q_path += "'";
    std::string q_branch = "'";
    for (char c : cfg.branch) { if (c == '\'') q_branch += "'\\''"; else q_branch += c; }
    q_branch += "'";
    add_cmd += q_path + " -b " + q_branch;

    GitOut add = run_git(launch_repo, add_cmd);
    if (add.exit_code != 0) {
        return Result<WorktreeReadyInfo>::failure(
            "git worktree add failed: " + add.stdout_text);
    }
    info.created_now = true;
    NEEDLE_LOG_INFO("worktree", "created worktree at %s on branch %s",
                    cfg.path.c_str(), cfg.branch.c_str());
    return Result<WorktreeReadyInfo>::success(info);
}

Result<void> WorktreeManager::remove(const std::string& path) {
    // We need a containing repo to run `git worktree remove`. Resolve via
    // git -C <path> worktree remove . — but `--git-dir` handling differs.
    // Simplest: shell into the path and ask git for the common-dir.
    GitOut common = run_git(path, "rev-parse --git-common-dir");
    if (common.exit_code != 0) {
        return Result<void>::failure("not inside a worktree: " + path);
    }
    // Run remove from the launch repo (parent worktree) using the path arg.
    std::string q_path = "'";
    for (char c : path) { if (c == '\'') q_path += "'\\''"; else q_path += c; }
    q_path += "'";
    GitOut out = run_git(path, "worktree remove " + q_path);
    if (out.exit_code != 0) {
        return Result<void>::failure("git worktree remove failed: " + out.stdout_text);
    }
    return Result<void>::success();
}

bool WorktreeManager::is_active_worktree(const std::string& path) {
    GitOut out = run_git(path, "rev-parse --is-inside-work-tree");
    if (out.exit_code != 0) return false;
    std::string trim = out.stdout_text;
    while (!trim.empty() && (trim.back() == '\n' || trim.back() == '\r')) {
        trim.pop_back();
    }
    return trim == "true";
}

} // namespace needle
