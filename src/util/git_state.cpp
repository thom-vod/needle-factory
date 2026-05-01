#include "needle/util/git_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace needle {

namespace {

// Run `git <args>` in `cwd`, capture stdout. Returns the trimmed stdout,
// or empty string on any failure. Best-effort by design; we never want a
// failed git invocation to surface as an error to callers.
std::string run_git_cmd(const std::string& cwd, const std::string& args) {
    // Build a shell-escaped invocation. We do the escaping minimally — the
    // caller passes a small set of fixed args, never user input.
    std::string cmd = "cd '";
    for (char c : cwd) {
        if (c == '\'') cmd += "'\\''";
        else cmd += c;
    }
    cmd += "' && git " + args + " 2>/dev/null";

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";

    std::string output;
    char buf[4096];
    while (true) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n == 0) break;
        output.append(buf, n);
    }

    int rc = pclose(fp);
    if (rc != 0) return "";

    // Trim trailing newlines.
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current += c;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

} // anonymous

GitStateSnapshot GitStateRecorder::capture(const std::string& cwd) {
    GitStateSnapshot snap;

    // Verify this is a git repo via rev-parse — fastest single check.
    std::string head = run_git_cmd(cwd, "rev-parse HEAD");
    if (head.empty()) {
        return snap;  // not a git repo, or empty HEAD, or git missing
    }
    snap.head = head;
    snap.valid = true;

    // Porcelain status: `XY <path>` per line. Skip blank/short lines.
    std::string porcelain = run_git_cmd(cwd, "status --porcelain");
    for (const auto& line : split_lines(porcelain)) {
        if (line.size() < 4) continue;
        std::string xy = line.substr(0, 2);
        std::string path = line.substr(3);
        if (xy == "??") {
            snap.untracked.push_back(path);
        } else if (xy[0] == 'M' || xy[1] == 'M' ||
                   xy[0] == 'A' || xy[1] == 'A' ||
                   xy[0] == 'D' || xy[1] == 'D' ||
                   xy[0] == 'R' || xy[1] == 'R') {
            snap.modified.push_back(path);
        }
    }
    return snap;
}

GitStateDelta GitStateRecorder::diff(const GitStateSnapshot& before,
                                     const GitStateSnapshot& after,
                                     const std::string& cwd) {
    GitStateDelta delta;
    if (!before.valid || !after.valid) return delta;

    // Commits added during the stage: HEAD moved.
    if (before.head != after.head) {
        std::string log = run_git_cmd(cwd, "log " + before.head + ".." + after.head +
                                          " --pretty=format:%H%x09%s");
        for (const auto& line : split_lines(log)) {
            auto tab = line.find('\t');
            GitCommitEntry e;
            if (tab == std::string::npos) {
                e.hash = line;
            } else {
                e.hash = line.substr(0, tab);
                e.subject = line.substr(tab + 1);
            }
            delta.commits_added.push_back(e);
        }
    }

    // Files newly untracked: present in `after.untracked` but not `before.untracked`.
    {
        std::set<std::string> before_set(before.untracked.begin(), before.untracked.end());
        for (const auto& p : after.untracked) {
            if (before_set.find(p) == before_set.end()) {
                delta.files_added_untracked.push_back(p);
            }
        }
    }

    // Files newly modified-but-uncommitted.
    {
        std::set<std::string> before_set(before.modified.begin(), before.modified.end());
        for (const auto& p : after.modified) {
            if (before_set.find(p) == before_set.end()) {
                delta.files_modified_uncommitted.push_back(p);
            }
        }
    }

    return delta;
}

nlohmann::json GitStateRecorder::to_json(const GitStateDelta& delta) {
    nlohmann::json j;

    nlohmann::json commits = nlohmann::json::array();
    for (const auto& c : delta.commits_added) {
        commits.push_back(nlohmann::json{{"hash", c.hash}, {"subject", c.subject}});
    }
    j["commits_added"] = commits;

    j["files_added_untracked"] = delta.files_added_untracked;
    j["files_modified_uncommitted"] = delta.files_modified_uncommitted;

    return j;
}

} // namespace needle
