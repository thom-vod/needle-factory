#include "needle/engine/troubleshoot_backup.h"

#include <fstream>

#include "needle/backend/process_runner.h"
#include "needle/platform/platform.h"

namespace needle {

namespace {

std::string branch_name_for(const std::string& run_id, const std::string& session_id) {
    return "auto/troubleshoot/backup/" + run_id + "-" + session_id;
}

Result<ProcessResult> run_git(const std::string& project_dir,
                              const std::vector<std::string>& args,
                              int timeout_ms = 60000) {
    NativeProcessRunner runner;
    return runner.run("git", args, project_dir, timeout_ms);
}

Result<std::string> git_ok_capture_stdout(const std::string& project_dir,
                                          const std::vector<std::string>& args,
                                          const std::string& action) {
    auto r = run_git(project_dir, args);
    if (!r.ok()) return Result<std::string>::failure(action + ": " + r.error());
    if (r.value().exit_code != 0) {
        return Result<std::string>::failure(action + ": " + r.value().stderr_output);
    }
    std::string out = r.value().stdout_output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return Result<std::string>::success(std::move(out));
}

Result<void> git_ok(const std::string& project_dir,
                    const std::vector<std::string>& args,
                    const std::string& action) {
    auto r = run_git(project_dir, args);
    if (!r.ok()) return Result<void>::failure(action + ": " + r.error());
    if (r.value().exit_code != 0) {
        return Result<void>::failure(action + ": " + r.value().stderr_output);
    }
    return Result<void>::success();
}

bool is_git_repo(const std::string& project_dir) {
    auto r = run_git(project_dir, {"rev-parse", "--git-dir"}, 10000);
    return r.ok() && r.value().exit_code == 0;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string acc;
    for (char c : text) {
        if (c == '\n') {
            if (!acc.empty() && acc.back() == '\r') acc.pop_back();
            out.push_back(acc);
            acc.clear();
        } else {
            acc.push_back(c);
        }
    }
    if (!acc.empty()) {
        if (acc.back() == '\r') acc.pop_back();
        out.push_back(acc);
    }
    return out;
}

bool write_lines(const std::string& path, const std::vector<std::string>& lines) {
    std::ofstream out(path);
    if (!out.is_open()) return false;
    for (const auto& line : lines) out << line << "\n";
    return true;
}

bool read_lines(const std::string& path, std::vector<std::string>& lines) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return true;
}

bool read_first_line(const std::string& path, std::string& value) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    if (!std::getline(in, value)) return false;
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return true;
}

} // namespace

Result<BackupInfo> TroubleshootBackup::capture(const std::string& project_dir,
                                               const std::string& run_id,
                                               const std::string& session_id,
                                               const std::string& session_dir) {
    if (project_dir.empty()) {
        return Result<BackupInfo>::failure("project_dir is required");
    }
    if (!is_git_repo(project_dir)) {
        return Result<BackupInfo>::failure(
            "backup-branch isolation requires a git repository at " + project_dir);
    }
    if (!platform::is_directory(session_dir) && !platform::mkdir_p(session_dir)) {
        return Result<BackupInfo>::failure("cannot create session dir: " + session_dir);
    }

    auto sha = git_ok_capture_stdout(project_dir, {"rev-parse", "HEAD"}, "git rev-parse HEAD");
    if (!sha.ok()) return Result<BackupInfo>::failure(sha.error());

    const std::string branch = branch_name_for(run_id, session_id);
    auto created = git_ok(project_dir,
                          {"branch", branch, sha.value()},
                          "git branch " + branch);
    if (!created.ok()) return Result<BackupInfo>::failure(created.error());

    auto untracked = git_ok_capture_stdout(
        project_dir,
        {"ls-files", "--others", "--exclude-standard"},
        "git ls-files --others");
    if (!untracked.ok()) {
        // Best effort: roll back the branch we just created, then surface the error.
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure(untracked.error());
    }

    if (!write_lines(session_dir + "/backup-base.txt", {sha.value()})) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("cannot write backup-base.txt");
    }
    if (!write_lines(session_dir + "/pre-untracked.txt", split_lines(untracked.value()))) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("cannot write pre-untracked.txt");
    }
    // Also record the branch name for symmetry / easy operator inspection.
    write_lines(session_dir + "/backup-branch.txt", {branch});

    BackupInfo info;
    info.branch = branch;
    info.base_sha = sha.value();
    return Result<BackupInfo>::success(info);
}

Result<RollbackReport> TroubleshootBackup::rollback(const std::string& project_dir,
                                                    const std::string& session_dir) {
    if (project_dir.empty()) {
        return Result<RollbackReport>::failure("project_dir is required");
    }
    if (!is_git_repo(project_dir)) {
        return Result<RollbackReport>::failure(
            "rollback requires a git repository at " + project_dir);
    }

    std::string base_sha;
    if (!read_first_line(session_dir + "/backup-base.txt", base_sha) || base_sha.empty()) {
        return Result<RollbackReport>::failure(
            "no backup-base.txt under " + session_dir + "; nothing to roll back");
    }
    std::string branch;
    if (!read_first_line(session_dir + "/backup-branch.txt", branch)) {
        // Not fatal — branch may have been cleaned up; we can still reset to base SHA.
        branch.clear();
    }

    std::vector<std::string> pre_untracked;
    read_lines(session_dir + "/pre-untracked.txt", pre_untracked);

    auto reset_result = git_ok(project_dir, {"reset", "--hard", base_sha},
                               "git reset --hard " + base_sha);
    if (!reset_result.ok()) return Result<RollbackReport>::failure(reset_result.error());

    auto now_untracked = git_ok_capture_stdout(
        project_dir,
        {"ls-files", "--others", "--exclude-standard"},
        "git ls-files --others");
    std::vector<std::string> drift;
    if (now_untracked.ok()) {
        std::vector<std::string> now_lines = split_lines(now_untracked.value());
        std::vector<std::string> pre_sorted = pre_untracked;
        std::sort(pre_sorted.begin(), pre_sorted.end());
        for (const auto& candidate : now_lines) {
            if (candidate.empty()) continue;
            if (!std::binary_search(pre_sorted.begin(), pre_sorted.end(), candidate)) {
                drift.push_back(candidate);
            }
        }
    }

    if (!branch.empty()) {
        // Best-effort branch cleanup; ignore failure (branch may already be gone).
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
    }

    RollbackReport report;
    report.branch = branch;
    report.base_sha = base_sha;
    report.untracked_drift = std::move(drift);
    return Result<RollbackReport>::success(std::move(report));
}

} // namespace needle
