#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace needle {

// Best-effort snapshot of a git working tree's state. All fields are
// populated via shell-out to `git`; if the directory isn't a git repo or
// the git invocation fails, the snapshot's `valid` flag is false and other
// fields are left empty.
struct GitStateSnapshot {
    bool valid = false;
    std::string head;                       // commit hash; empty if invalid
    std::vector<std::string> untracked;     // paths from `git status --porcelain` "??"
    std::vector<std::string> modified;      // paths from `git status --porcelain` " M" / "M "
};

struct GitCommitEntry {
    std::string hash;
    std::string subject;
};

// Difference between two snapshots. Captures what landed during a stage —
// commits added, files newly untracked or modified — so operators don't
// have to do `git status` archaeology after every failure.
struct GitStateDelta {
    std::vector<GitCommitEntry> commits_added;
    std::vector<std::string> files_added_untracked;
    std::vector<std::string> files_modified_uncommitted;
};

// Tiny utility for snapshotting git state. Intentionally best-effort: every
// failure mode (non-git dir, git not on PATH, broken HEAD, etc.) yields a
// `valid=false` snapshot rather than an exception, so callers can ignore
// the outcome without special-casing each failure.
class GitStateRecorder {
public:
    // Snapshot HEAD + porcelain status of the working tree at `cwd`.
    // Returns a snapshot with `valid=false` if anything goes wrong.
    static GitStateSnapshot capture(const std::string& cwd);

    // Compute the delta between two snapshots. Both must be valid; if
    // either is invalid the returned delta is empty.
    static GitStateDelta diff(const GitStateSnapshot& before,
                              const GitStateSnapshot& after,
                              const std::string& cwd);

    // Serialise a delta to JSON for inclusion in stage status / context.
    static nlohmann::json to_json(const GitStateDelta& delta);
};

} // namespace needle
