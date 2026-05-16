#include "needle/worktree/strategy.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "needle/backend/process_runner.h"
#include "needle/platform/platform.h"

namespace needle {

namespace {

std::string basename_of(const std::string& path) {
    size_t end = path.find_last_not_of("/\\");
    if (end == std::string::npos) return path;
    size_t start = path.find_last_of("/\\", end);
    return path.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
}

std::string run_dir_from_session_dir(const std::string& session_dir) {
    size_t session_slash = session_dir.find_last_of("/\\");
    if (session_slash == std::string::npos) return "";
    std::string troubleshoot_dir = session_dir.substr(0, session_slash);
    if (basename_of(troubleshoot_dir) != "troubleshoot") return "";
    size_t run_slash = troubleshoot_dir.find_last_of("/\\");
    if (run_slash == std::string::npos) return "";
    return troubleshoot_dir.substr(0, run_slash);
}

std::string branch_for_run(const std::string& run_id) {
    return "auto/troubleshoot/" + run_id;
}

Result<ProcessResult> run_git(const std::string& project_dir,
                              const std::vector<std::string>& args) {
    NativeProcessRunner runner;
    return runner.run("git", args, project_dir, 120000);
}

Result<void> require_git_ok(const std::string& project_dir,
                            const std::vector<std::string>& args,
                            const std::string& action) {
    auto r = run_git(project_dir, args);
    if (!r.ok()) return Result<void>::failure(action + ": " + r.error());
    if (r.value().exit_code != 0) {
        return Result<void>::failure(action + ": " + r.value().stderr_output);
    }
    return Result<void>::success();
}

Result<std::string> find_worktree_path(const std::string& project_dir,
                                       const std::string& run_id) {
    const std::string branch = branch_for_run(run_id);
    auto r = run_git(project_dir, {"worktree", "list", "--porcelain"});
    if (!r.ok()) return Result<std::string>::failure("git worktree list: " + r.error());
    if (r.value().exit_code != 0) {
        return Result<std::string>::failure("git worktree list: " + r.value().stderr_output);
    }
    std::istringstream in(r.value().stdout_output);
    std::string line;
    std::string current_path;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.pop_back();
        if (line.rfind("worktree ", 0) == 0) {
            current_path = line.substr(9);
        } else if (line == "branch refs/heads/" + branch && !current_path.empty()) {
            return Result<std::string>::success(current_path);
        }
    }
    const std::string needle_dir = project_dir + "/.needle";
    if (platform::is_directory(needle_dir)) {
        for (const auto& stem : platform::list_directory(needle_dir)) {
            std::string candidate = needle_dir + "/" + stem + "/troubleshoot-wt-" + run_id;
            if (platform::is_directory(candidate)) {
                return Result<std::string>::success(candidate);
            }
        }
    }
    return Result<std::string>::failure("troubleshoot worktree not found for " + run_id);
}

} // namespace

WorktreeStrategy worktree_strategy_from_string(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "auto")   return WorktreeStrategy::Auto;
    if (lower == "manual") return WorktreeStrategy::Manual;
    return WorktreeStrategy::Off;
}

std::string to_string(WorktreeStrategy strategy) {
    switch (strategy) {
        case WorktreeStrategy::Auto:   return "auto";
        case WorktreeStrategy::Manual: return "manual";
        case WorktreeStrategy::Off:    return "off";
    }
    return "off";
}

Result<std::string> interpolate_template(const std::string& tmpl,
                                         const std::map<std::string, std::string>& params) {
    std::string out;
    out.reserve(tmpl.size());

    size_t i = 0;
    while (i < tmpl.size()) {
        if (i + 1 < tmpl.size() && tmpl[i] == '$' && tmpl[i + 1] == '{') {
            size_t end = tmpl.find('}', i + 2);
            if (end == std::string::npos) {
                return Result<std::string>::failure(
                    "unterminated `${` in template: " + tmpl);
            }
            std::string var = tmpl.substr(i + 2, end - i - 2);
            auto it = params.find(var);
            if (it == params.end()) {
                return Result<std::string>::failure(
                    "missing parameter `${" + var + "}` in template: " + tmpl);
            }
            out += it->second;
            i = end + 1;
        } else {
            out += tmpl[i];
            ++i;
        }
    }
    return Result<std::string>::success(std::move(out));
}

Result<std::string> TroubleshootWorktree::create(const std::string& project_dir,
                                                 const std::string& run_id,
                                                 const std::string& session_dir) {
    if (project_dir.empty()) return Result<std::string>::failure("project_dir is required");
    if (run_id.empty()) return Result<std::string>::failure("run_id is required");

    std::string run_dir = run_dir_from_session_dir(session_dir);
    std::string stem = basename_of(run_dir);
    if (stem.empty()) return Result<std::string>::failure("cannot infer DOT stem from session dir");

    const std::string branch = branch_for_run(run_id);
    const std::string worktree_path = project_dir + "/.needle/" + stem + "/troubleshoot-wt-" + run_id;
    platform::mkdir_p(project_dir + "/.needle/" + stem);

    auto added = require_git_ok(project_dir,
                                {"worktree", "add", "-b", branch, worktree_path},
                                "git worktree add");
    if (!added.ok()) return Result<std::string>::failure(added.error());

    const std::string record_dir = session_dir + "/worktree";
    if (!platform::mkdir_p(record_dir)) {
        return Result<std::string>::failure("cannot create " + record_dir);
    }
    std::ofstream out(record_dir + "/branch.txt");
    if (!out.is_open()) return Result<std::string>::failure("cannot write worktree branch record");
    out << "path=" << worktree_path << "\n";
    out << "branch=" << branch << "\n";
    return Result<std::string>::success(worktree_path);
}

Result<void> TroubleshootWorktree::apply(const std::string& project_dir,
                                         const std::string& run_id) {
    const std::string branch = branch_for_run(run_id);
    auto path = find_worktree_path(project_dir, run_id);
    if (!path.ok()) return Result<void>::failure(path.error());
    auto merged = require_git_ok(project_dir, {"merge", "--ff-only", branch}, "git merge --ff-only");
    if (!merged.ok()) return merged;
    return require_git_ok(project_dir, {"worktree", "remove", "--force", path.value()}, "git worktree remove");
}

Result<void> TroubleshootWorktree::discard(const std::string& project_dir,
                                           const std::string& run_id) {
    auto path = find_worktree_path(project_dir, run_id);
    if (path.ok()) {
        auto removed = require_git_ok(project_dir, {"worktree", "remove", "--force", path.value()}, "git worktree remove");
        if (!removed.ok()) return removed;
    }
    return require_git_ok(project_dir, {"branch", "-D", branch_for_run(run_id)}, "git branch -D");
}

} // namespace needle
