#include "needle/engine/troubleshoot_backup.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "needle/backend/process_runner.h"
#include "needle/platform/platform.h"

namespace needle {

namespace {

std::string branch_name_for(const std::string& run_id, const std::string& session_id) {
    return "auto/troubleshoot/backup/" + run_id + "-" + session_id;
}

bool is_valid_sha(const std::string& sha) {
    if (sha.size() != 40) return false;
    return std::all_of(sha.begin(), sha.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string join_names(const std::vector<std::string>& names) {
    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out << ", ";
        out << names[i];
    }
    return out.str();
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
            if (!acc.empty()) out.push_back(acc);
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

Result<std::string> current_branch_state(const std::string& project_dir,
                                         const std::string& detached_sha = std::string()) {
    auto branch = run_git(project_dir, {"symbolic-ref", "--short", "HEAD"}, 10000);
    if (!branch.ok()) {
        return Result<std::string>::failure("git symbolic-ref --short HEAD: " + branch.error());
    }
    if (branch.value().exit_code == 0) {
        std::string out = branch.value().stdout_output;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (!out.empty()) return Result<std::string>::success(std::move(out));
    }

    std::string sha = detached_sha;
    if (sha.empty()) {
        auto current_sha = git_ok_capture_stdout(project_dir, {"rev-parse", "HEAD"},
                                                 "git rev-parse HEAD");
        if (!current_sha.ok()) return Result<std::string>::failure(current_sha.error());
        sha = current_sha.value();
    }
    if (!is_valid_sha(sha)) {
        return Result<std::string>::failure("SHA validation failed for detached HEAD: " + sha);
    }
    return Result<std::string>::success("__detached__:" + sha);
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
        if (!line.empty()) lines.push_back(line);
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

    auto current_branch = current_branch_state(project_dir, sha.value());
    if (!current_branch.ok()) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure(current_branch.error());
    }
    if (!is_valid_sha(sha.value())) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("SHA validation failed for backup base: " + sha.value());
    }

    auto untracked = git_ok_capture_stdout(
        project_dir,
        {"ls-files", "--others", "--exclude-standard"},
        "git ls-files --others");
    if (!untracked.ok()) {
        // Best effort: roll back the branch we just created, then surface the error.
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure(untracked.error());
    }
    auto modified = git_ok_capture_stdout(
        project_dir,
        {"diff", "--name-only", "HEAD"},
        "git diff --name-only HEAD");
    if (!modified.ok()) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure(modified.error());
    }

    if (!write_lines(session_dir + "/backup-base.txt", {sha.value()})) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("cannot write backup-base.txt");
    }
    if (!write_lines(session_dir + "/pre-untracked.txt", split_lines(untracked.value()))) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("cannot write pre-untracked.txt");
    }
    if (!write_lines(session_dir + "/backup-branch.txt", {branch})) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("cannot write backup-branch.txt");
    }
    if (!write_lines(session_dir + "/current-branch.txt", {current_branch.value()})) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("cannot write current-branch.txt");
    }
    auto pre_modified = split_lines(modified.value());
    if (!write_lines(session_dir + "/pre-modified.txt", pre_modified)) {
        git_ok(project_dir, {"branch", "-D", branch}, "git branch -D");
        return Result<BackupInfo>::failure("cannot write pre-modified.txt");
    }

    BackupInfo info;
    info.branch = branch;
    info.base_sha = sha.value();
    info.current_branch = current_branch.value();
    info.pre_modified = std::move(pre_modified);
    return Result<BackupInfo>::success(info);
}

Result<std::vector<std::string>> TroubleshootBackup::record_agent_touch(
    const std::string& project_dir,
    const std::string& base_sha,
    const std::string& session_dir) {
    if (!is_valid_sha(base_sha)) {
        return Result<std::vector<std::string>>::failure(
            "SHA validation failed for backup base: " + base_sha);
    }

    auto modified = git_ok_capture_stdout(
        project_dir,
        {"diff", "--name-only", base_sha},
        "git diff --name-only " + base_sha);
    if (!modified.ok()) {
        return Result<std::vector<std::string>>::failure(modified.error());
    }

    auto lines = split_lines(modified.value());
    if (!write_lines(session_dir + "/agent-modified.txt", lines)) {
        return Result<std::vector<std::string>>::failure("cannot write agent-modified.txt");
    }
    return Result<std::vector<std::string>>::success(std::move(lines));
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
    if (!is_valid_sha(base_sha)) {
        return Result<RollbackReport>::failure(
            "SHA validation failed for backup-base.txt: " + base_sha);
    }
    std::string branch;
    if (!read_first_line(session_dir + "/backup-branch.txt", branch)) {
        // Not fatal — branch may have been cleaned up; we can still reset to base SHA.
        branch.clear();
    }
    // m-e: legacy sessions captured on pre-SPRINT-017 builds lack
    // current-branch.txt and pre-modified.txt. We can still roll them
    // back safely if the operator's tree is clean — there's no recorded
    // baseline to compare drift against, so we require zero divergence.
    std::string recorded_branch;
    const bool legacy_no_branch = !read_first_line(session_dir + "/current-branch.txt",
                                                   recorded_branch) || recorded_branch.empty();

    std::vector<std::string> pre_untracked;
    read_lines(session_dir + "/pre-untracked.txt", pre_untracked);
    std::vector<std::string> pre_modified;
    const bool legacy_no_pre_modified =
        !read_lines(session_dir + "/pre-modified.txt", pre_modified);
    const bool legacy_session = legacy_no_branch || legacy_no_pre_modified;

    auto current_branch = current_branch_state(project_dir);
    if (!current_branch.ok()) return Result<RollbackReport>::failure(current_branch.error());
    if (legacy_no_branch) {
        // No recorded branch — inherit the current one for the report; skip
        // the drift check.
        recorded_branch = current_branch.value();
    } else if (current_branch.value() != recorded_branch) {
        return Result<RollbackReport>::failure(
            "rollback refused: current branch " + current_branch.value() +
            " does not match recorded branch " + recorded_branch);
    }

    auto current_modified_result = git_ok_capture_stdout(
        project_dir,
        {"diff", "--name-only", "HEAD"},
        "git diff --name-only HEAD");
    if (!current_modified_result.ok()) {
        return Result<RollbackReport>::failure(current_modified_result.error());
    }

    std::set<std::string> allowed(pre_modified.begin(), pre_modified.end());
    std::vector<std::string> agent_touched;
    const std::string agent_modified_path = session_dir + "/agent-modified.txt";
    if (platform::file_exists(agent_modified_path)) {
        if (!read_lines(agent_modified_path, agent_touched)) {
            return Result<RollbackReport>::failure(
                "cannot read agent-modified.txt under " + session_dir);
        }
    } else if (!legacy_session) {
        // SPRINT-017+ session that should have agent-modified.txt but
        // doesn't (e.g. agent crashed before record_agent_touch). Fall
        // back to the committed-diff form.
        auto agent_touched_result = git_ok_capture_stdout(
            project_dir,
            {"diff", "--name-only", base_sha, "HEAD"},
            "git diff --name-only " + base_sha + " HEAD");
        if (!agent_touched_result.ok()) {
            return Result<RollbackReport>::failure(agent_touched_result.error());
        }
        agent_touched = split_lines(agent_touched_result.value());
    }
    // For legacy sessions, agent_touched stays empty AND pre_modified
    // stays empty (since pre-modified.txt was absent), so the only
    // accepted state is a fully clean working tree.
    allowed.insert(agent_touched.begin(), agent_touched.end());

    std::vector<std::string> divergent;
    for (const auto& modified : split_lines(current_modified_result.value())) {
        if (allowed.find(modified) == allowed.end()) divergent.push_back(modified);
    }
    if (!divergent.empty()) {
        std::string msg = legacy_session
            ? "rollback refused: legacy session has no recorded baseline; "
              "commit or stash dirty files first. Divergent: "
            : "rollback refused: divergent dirty files: ";
        return Result<RollbackReport>::failure(msg + join_names(divergent));
    }

    // m-f: pre-capture dirty files are legitimate reset targets per design,
    // but `git reset --hard` destroys them silently. Capture the intersection
    // of (still-dirty now) ∩ (recorded pre_modified) so the caller can warn
    // the operator about which of their pre-agent edits were affected.
    std::vector<std::string> reset_pre_modified;
    {
        std::set<std::string> pre_set(pre_modified.begin(), pre_modified.end());
        for (const auto& f : split_lines(current_modified_result.value())) {
            if (!f.empty() && pre_set.count(f)) reset_pre_modified.push_back(f);
        }
    }

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
    report.current_branch = recorded_branch;
    report.untracked_drift = std::move(drift);
    report.reset_pre_modified = std::move(reset_pre_modified);
    return Result<RollbackReport>::success(std::move(report));
}

} // namespace needle
