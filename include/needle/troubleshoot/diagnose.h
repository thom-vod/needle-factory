#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace needle {

enum class FailureKind {
    Unknown,
    IdleStallNoWorkSalvageable,
    IdleStallWorkOnDisk,
    IdleStallWorkCommitted,
    WallClockWithProgress,
    WallClockWithoutOwnProgress,
    SelfExitError,
    PromptBlowup,
    RolePromptConflict,
    VariableCorrupted,
    OrphanedSubprocesses,
};

std::string failure_kind_string(FailureKind k);

struct CommitEntry {
    std::string hash;
    std::string subject;
};

struct DiagnosisSignals {
    std::string current_node;
    std::vector<std::string> completed_nodes;
    std::string last_outcome_status;
    std::string graph_file;

    std::string failed_node;
    std::string status_status;
    std::string status_output;
    int prompt_size_kb = 0;
    bool prompt_md_present = false;
    bool response_md_present = false;
    int response_md_bytes = 0;
    std::string timeout_kind;
    bool timed_out = false;
    int exit_code = 0;
    int stdout_bytes = 0;

    std::vector<CommitEntry> commits_added;
    std::vector<CommitEntry> own_commits;
    std::string start_commit_hash;
    std::vector<std::string> files_added_untracked;
    std::vector<std::string> files_modified_uncommitted;

    std::vector<std::string> role_keywords_found;
    std::vector<std::string> impl_keywords_found;
    std::vector<std::string> unresolved_vars;

    int engine_pid = 0;
    bool engine_pid_alive = false;
    std::vector<int> descendant_pids;
};

struct ParallelBranchDiagnosis {
    std::string child_node_id;
    FailureKind kind = FailureKind::Unknown;
    DiagnosisSignals signals;
};

struct DiagnosisReport {
    DiagnosisSignals signals;
    FailureKind kind = FailureKind::Unknown;
    std::vector<ParallelBranchDiagnosis> children;
};

class Diagnose {
public:
    static DiagnosisSignals collect(const std::string& run_dir,
                                    const std::string& failed_node = "");

    static DiagnosisReport collect_report(const std::string& run_dir,
                                          const std::string& failed_node = "");

    static FailureKind classify(const DiagnosisSignals& signals);

    static std::string render_markdown(const DiagnosisSignals& signals,
                                       FailureKind kind);

    static std::string render_markdown(const DiagnosisReport& report);
};

} // namespace needle
