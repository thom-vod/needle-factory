#pragma once

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

namespace needle {

// Failure-mode classification used by the troubleshoot agent. Matches the
// catalogue in ~/notes/needle-troubleshoot-agent-proposal.md §5.3.
enum class FailureKind {
    Unknown,
    IdleStallNoWorkSalvageable,    // idle stall, no commits, no working-tree delta
    IdleStallWorkOnDisk,            // idle stall, but uncommitted edits exist
    IdleStallWorkCommitted,         // idle stall, commits added during stage
    WallClockWithProgress,          // exit_code=0 + commits, summary truncated
    SelfExitError,                  // non-timeout failure, short error output
    PromptBlowup,                   // prompt size > threshold
};

std::string failure_kind_string(FailureKind k);

struct CommitEntry {
    std::string hash;
    std::string subject;
};

// Snapshot of all the diagnostic signals the classifier consumes. Populated
// from a run directory (`<logs_root>/<run-id>/`) by Diagnose::collect.
struct DiagnosisSignals {
    // From checkpoint.json
    std::string current_node;
    std::vector<std::string> completed_nodes;
    std::string last_outcome_status;

    // From stages/<failed_node>/
    std::string failed_node;
    std::string status_status;       // "FAILURE" | "SUCCESS" | etc.
    std::string status_output;
    int prompt_size_kb = 0;
    bool prompt_md_present = false;
    bool response_md_present = false;
    int response_md_bytes = 0;
    std::string timeout_kind;        // "wall_clock" | "idle" | "" (none)
    bool timed_out = false;
    int exit_code = 0;
    int stdout_bytes = 0;

    // git_state pulled from status.json (N2 surfaces this)
    std::vector<CommitEntry> commits_added;
    std::vector<std::string> files_added_untracked;
    std::vector<std::string> files_modified_uncommitted;
};

// Pulls signals from a run directory. Best-effort — missing files yield
// default-initialised fields rather than errors so the classifier can still
// run on partial data.
class Diagnose {
public:
    // run_dir typically `<logs_root>/<run-id>/`. failed_node optional — if
    // empty, we infer it from checkpoint's current_node.
    static DiagnosisSignals collect(const std::string& run_dir,
                                    const std::string& failed_node = "");

    // Apply the pattern catalogue to classify the failure.
    static FailureKind classify(const DiagnosisSignals& signals);

    // Render a human-readable markdown report. Mirrors the shape from
    // troubleshoot-agent-proposal.md §5.4.
    static std::string render_markdown(const DiagnosisSignals& signals,
                                       FailureKind kind);
};

} // namespace needle
