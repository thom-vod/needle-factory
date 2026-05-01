#include "needle/troubleshoot/diagnose.h"

#include "needle/util/fs_helpers.h"

#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace needle {

namespace {

bool file_exists_local(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

int file_size_bytes(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int>(st.st_size);
}

} // anonymous

std::string failure_kind_string(FailureKind k) {
    switch (k) {
        case FailureKind::IdleStallNoWorkSalvageable: return "idle_stall_clean";
        case FailureKind::IdleStallWorkOnDisk:        return "idle_stall_with_uncommitted_work";
        case FailureKind::IdleStallWorkCommitted:     return "idle_stall_with_committed_work";
        case FailureKind::WallClockWithProgress:      return "wall_clock_with_progress";
        case FailureKind::SelfExitError:              return "self_exit_error";
        case FailureKind::PromptBlowup:               return "prompt_blowup";
        case FailureKind::Unknown:                    return "unknown";
    }
    return "unknown";
}

DiagnosisSignals Diagnose::collect(const std::string& run_dir,
                                   const std::string& failed_node_in) {
    DiagnosisSignals s;

    // Checkpoint
    std::string cp_path = run_dir + "/checkpoint.json";
    if (file_exists_local(cp_path)) {
        try {
            std::ifstream in(cp_path);
            nlohmann::json j;
            in >> j;
            s.current_node = j.value("current_node", "");
            if (j.count("completed_nodes")) {
                for (const auto& n : j["completed_nodes"]) {
                    s.completed_nodes.push_back(n.get<std::string>());
                }
            }
            if (j.count("context") && j["context"].is_object()) {
                if (j["context"].count("needle.last_outcome.status")) {
                    s.last_outcome_status =
                        j["context"]["needle.last_outcome.status"].get<std::string>();
                }
            }
        } catch (...) {
            // Best-effort
        }
    }

    s.failed_node = failed_node_in.empty() ? s.current_node : failed_node_in;
    if (s.failed_node.empty()) return s;

    // Stage status.json
    std::string stage_dir = run_dir + "/stages/" + s.failed_node;
    std::string status_path = stage_dir + "/status.json";
    if (file_exists_local(status_path)) {
        try {
            std::ifstream in(status_path);
            nlohmann::json j;
            in >> j;
            s.status_status = j.value("status", "");
            s.status_output = j.value("output", "");
            s.timeout_kind = j.value("timeout_kind", "");
            s.timed_out = !s.timeout_kind.empty();
            if (j.count("git_state") && j["git_state"].is_object()) {
                const auto& gs = j["git_state"];
                if (gs.count("commits_added")) {
                    for (const auto& c : gs["commits_added"]) {
                        CommitEntry e;
                        e.hash = c.value("hash", "");
                        e.subject = c.value("subject", "");
                        s.commits_added.push_back(std::move(e));
                    }
                }
                if (gs.count("files_added_untracked")) {
                    for (const auto& f : gs["files_added_untracked"]) {
                        s.files_added_untracked.push_back(f.get<std::string>());
                    }
                }
                if (gs.count("files_modified_uncommitted")) {
                    for (const auto& f : gs["files_modified_uncommitted"]) {
                        s.files_modified_uncommitted.push_back(f.get<std::string>());
                    }
                }
            }
        } catch (...) {
            // Best-effort
        }
    }

    // Prompt + response
    std::string prompt_path = stage_dir + "/prompt.md";
    s.prompt_md_present = file_exists_local(prompt_path);
    if (s.prompt_md_present) {
        s.prompt_size_kb = file_size_bytes(prompt_path) / 1024;
    }

    std::string response_path = stage_dir + "/response.md";
    s.response_md_present = file_exists_local(response_path);
    if (s.response_md_present) {
        s.response_md_bytes = file_size_bytes(response_path);
        s.stdout_bytes = s.response_md_bytes;  // approximation
    }

    // Infer exit_code from status_status. We don't have the raw exit code on
    // disk; status_status="FAILURE" implies a nonzero exit OR a timeout.
    s.exit_code = (s.status_status == "FAILURE") ? 1 : 0;

    return s;
}

FailureKind Diagnose::classify(const DiagnosisSignals& signals) {
    // Pattern matching follows ~/notes/needle-troubleshoot-agent-proposal.md
    // §5.3 in priority order.

    if (signals.prompt_size_kb >= 200) {
        return FailureKind::PromptBlowup;
    }

    if (signals.timed_out) {
        bool has_commits = !signals.commits_added.empty();
        bool has_uncommitted = !signals.files_added_untracked.empty() ||
                               !signals.files_modified_uncommitted.empty();

        if (signals.timeout_kind == "wall_clock" && has_commits) {
            return FailureKind::WallClockWithProgress;
        }
        if (signals.timeout_kind == "idle" || signals.timeout_kind == "wall_clock") {
            if (has_commits) return FailureKind::IdleStallWorkCommitted;
            if (has_uncommitted) return FailureKind::IdleStallWorkOnDisk;
            return FailureKind::IdleStallNoWorkSalvageable;
        }
    }

    if (signals.status_status == "FAILURE") {
        return FailureKind::SelfExitError;
    }

    return FailureKind::Unknown;
}

std::string Diagnose::render_markdown(const DiagnosisSignals& s, FailureKind kind) {
    std::stringstream out;
    out << "# Needle Troubleshoot — " << s.failed_node << "\n\n";

    out << "## Diagnosis\n";
    out << "- **Failure kind:** " << failure_kind_string(kind) << "\n";
    out << "- **Stage:** " << s.failed_node << "\n";
    out << "- **Status:** " << (s.status_status.empty() ? "(unknown)" : s.status_status) << "\n";
    if (s.timed_out) {
        out << "- **Timeout kind:** " << s.timeout_kind << "\n";
    }
    out << "- **Prompt size:** " << s.prompt_size_kb << " KB\n";
    out << "- **Response size:** " << s.response_md_bytes << " bytes\n";
    out << "- **Commits added during stage:** " << s.commits_added.size();
    if (!s.commits_added.empty()) {
        out << "\n";
        for (const auto& c : s.commits_added) {
            out << "  - `" << c.hash.substr(0, 12) << "` " << c.subject << "\n";
        }
    } else {
        out << " (none)\n";
    }
    out << "- **Untracked files added:** " << s.files_added_untracked.size();
    if (!s.files_added_untracked.empty()) {
        out << "\n";
        for (const auto& f : s.files_added_untracked) out << "  - " << f << "\n";
    } else {
        out << " (none)\n";
    }
    out << "- **Modified-but-uncommitted files:** " << s.files_modified_uncommitted.size();
    if (!s.files_modified_uncommitted.empty()) {
        out << "\n";
        for (const auto& f : s.files_modified_uncommitted) out << "  - " << f << "\n";
    } else {
        out << " (none)\n";
    }
    out << "\n";

    out << "## Likely root cause\n";
    switch (kind) {
        case FailureKind::IdleStallNoWorkSalvageable:
            out << "Agent went silent and produced no output before being killed. "
                   "No commits or uncommitted edits to salvage. Reset the stage "
                   "for a clean retry; consider lowering fidelity / extending "
                   "timeout / injecting the anti-stall preamble.\n";
            break;
        case FailureKind::IdleStallWorkOnDisk:
            out << "Agent stalled but left work on disk uncommitted. Build the "
                   "working tree to confirm it compiles; if clean, salvage with "
                   "a commit + advance. If broken, repair before salvaging.\n";
            break;
        case FailureKind::IdleStallWorkCommitted:
            out << "Agent committed during the stage but the wrapper didn't "
                   "see SUCCESS (likely killed mid-summary). Mark the stage "
                   "SUCCESS and advance — the work landed.\n";
            break;
        case FailureKind::WallClockWithProgress:
            out << "Stage hit wall-clock, but committed work and produced output. "
                   "Mark SUCCESS + advance.\n";
            break;
        case FailureKind::SelfExitError:
            out << "Process exited non-zero with little output. Read response.md "
                   "for the error; common patterns include analyzer errors and "
                   "missing project references.\n";
            break;
        case FailureKind::PromptBlowup:
            out << "Prompt size " << s.prompt_size_kb << " KB exceeds the warn "
                   "threshold (100 KB). Lower fidelity on remaining codergen "
                   "nodes (`fidelity=\"summary:high\"`) and resume.\n";
            break;
        case FailureKind::Unknown:
            out << "Pattern matcher couldn't classify; surface to operator.\n";
            break;
    }
    out << "\n";

    out << "## Proposed actions (v1: diagnose only)\n";
    out << "This is a v1 diagnose-only invocation — no actions are being applied. "
           "v2 (salvage commits) and v3 (checkpoint advancement) ship in later "
           "sprints. For now, the report is informational; operators can use "
           "`needle stage mark` / `needle stage advance` (Sprint 3) to apply "
           "checkpoint mutations by hand.\n";

    return out.str();
}

} // namespace needle
