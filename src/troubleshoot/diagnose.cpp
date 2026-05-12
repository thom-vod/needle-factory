#include "needle/troubleshoot/diagnose.h"

#include "needle/model/graph.h"
#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/platform/platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <memory>
#include <sys/stat.h>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace needle {
namespace {

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

bool file_exists_local(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

int file_size_bytes(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int>(st.st_size);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

int run_command_exit_code(const std::string& cmd) {
    return std::system(cmd.c_str());
}

std::string infer_project_dir(const std::string& run_dir) {
    const std::string marker = "/.needle/";
    size_t pos = run_dir.find(marker);
    if (pos != std::string::npos) {
        return run_dir.substr(0, pos);
    }
    size_t slash = run_dir.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return run_dir.substr(0, slash);
}

bool is_own_commit(const std::string& project_dir,
                   const std::string& start_commit,
                   const std::string& commit_hash,
                   std::map<std::string, bool>& cache) {
    if (start_commit.empty() || commit_hash.empty()) return true;
    auto it = cache.find(commit_hash);
    if (it != cache.end()) return it->second;

    std::string cmd = "git -C " + shell_quote(project_dir)
                    + " merge-base --is-ancestor "
                    + shell_quote(commit_hash) + " " + shell_quote(start_commit)
                    + " >/dev/null 2>&1";
    bool own = (run_command_exit_code(cmd) != 0);
    cache[commit_hash] = own;
    return own;
}

std::string strip_fences_and_quotes(const std::string& input, bool strip_quoted_strings) {
    std::stringstream out;
    std::istringstream in(input);
    std::string line;
    bool in_fence = false;

    while (std::getline(in, line)) {
        std::string l = trim(line);
        if (l.size() >= 3 && l.substr(0, 3) == "```") {
            in_fence = !in_fence;
            continue;
        }
        if (in_fence) continue;

        if (!strip_quoted_strings) {
            out << line << "\n";
            continue;
        }

        bool in_quote = false;
        std::string filtered;
        filtered.reserve(line.size());
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"') {
                in_quote = !in_quote;
                continue;
            }
            if (!in_quote) filtered.push_back(c);
        }
        out << filtered << "\n";
    }
    return out.str();
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    std::string h = to_lower(haystack);
    std::string n = to_lower(needle);
    return h.find(n) != std::string::npos;
}

void scan_prompt_fingerprints(const std::string& prompt_text,
                              std::vector<std::string>& role_found,
                              std::vector<std::string>& impl_found) {
    static const char* kRoleKeywords[] = {
        "reviewer mode",
        "docs mode",
        "must not call write or edit",
        "operating in"
    };
    static const char* kImplKeywords[] = {
        "commit your changes",
        "run the project's test suite",
        "after completing your implementation"
    };

    std::string filtered = strip_fences_and_quotes(prompt_text, true);
    for (const char* kw : kRoleKeywords) {
        if (contains_ci(filtered, kw)) role_found.push_back(kw);
    }
    for (const char* kw : kImplKeywords) {
        if (contains_ci(filtered, kw)) impl_found.push_back(kw);
    }
}

void scan_unresolved_vars(const std::string& run_dir,
                          const std::string& failed_node,
                          const std::string& prompt_text,
                          std::vector<std::string>& out_vars) {
    std::set<std::string> names;

    std::string filtered = strip_fences_and_quotes(prompt_text, false);
    size_t pos = 0;
    while ((pos = filtered.find("$var.", pos)) != std::string::npos) {
        size_t start = pos + 5;
        size_t end = start;
        while (end < filtered.size()) {
            char c = filtered[end];
            bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
            if (!ok) break;
            end++;
        }
        if (end > start) names.insert(filtered.substr(start, end - start));
        pos = start;
    }

    std::vector<std::string> log_candidates = {
        run_dir + "/debug.log",
        run_dir + "/stages/" + failed_node + "/debug.log"
    };
    for (const auto& path : log_candidates) {
        if (!file_exists_local(path)) continue;
        std::string log = read_file(path);
        if (log.find("[VARIABLE_UNRESOLVED]") == std::string::npos) continue;
        std::istringstream iss(log);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("[VARIABLE_UNRESOLVED]") == std::string::npos) continue;
            size_t p = line.find("$var.");
            if (p == std::string::npos) continue;
            size_t start = p + 5;
            size_t end = start;
            while (end < line.size()) {
                char c = line[end];
                bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
                if (!ok) break;
                end++;
            }
            if (end > start) names.insert(line.substr(start, end - start));
        }
    }

    out_vars.assign(names.begin(), names.end());
}

bool process_alive(int pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid, 0) == 0;
#endif
}

std::unique_ptr<Graph> read_graph_from_file(const std::string& graph_file) {
    if (graph_file.empty() || !file_exists_local(graph_file)) return nullptr;
    std::string dot_source = read_file(graph_file);
    if (dot_source.empty()) return nullptr;

    DotParser parser(dot_source);
    auto ast = parser.parse();
    if (!ast.ok()) return nullptr;

    GraphBuilder builder;
    auto graph_res = builder.build(ast.value());
    if (!graph_res.ok()) return nullptr;
    return std::unique_ptr<Graph>(new Graph(graph_res.value()));
}

void fill_stage_signals(DiagnosisSignals& s,
                        const std::string& run_dir,
                        const std::string& failed_node) {
    s.failed_node = failed_node;
    if (s.failed_node.empty()) return;

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
            s.cherry_pick_conflict = j.count("cherry_pick_conflict") > 0;
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
        }
    }

    std::string start_commit_path = stage_dir + "/start_commit.txt";
    if (file_exists_local(start_commit_path)) {
        s.start_commit_hash = trim(read_file(start_commit_path));
    }

    std::string prompt_path = stage_dir + "/prompt.md";
    s.prompt_md_present = file_exists_local(prompt_path);
    std::string prompt_text;
    if (s.prompt_md_present) {
        s.prompt_size_kb = file_size_bytes(prompt_path) / 1024;
        prompt_text = read_file(prompt_path);
        scan_prompt_fingerprints(prompt_text, s.role_keywords_found, s.impl_keywords_found);
        scan_unresolved_vars(run_dir, s.failed_node, prompt_text, s.unresolved_vars);
    }

    std::string response_path = stage_dir + "/response.md";
    s.response_md_present = file_exists_local(response_path);
    if (s.response_md_present) {
        s.response_md_bytes = file_size_bytes(response_path);
        s.stdout_bytes = s.response_md_bytes;
    }

    s.exit_code = (s.status_status == "FAILURE") ? 1 : 0;

    std::string project_dir = infer_project_dir(run_dir);
    std::map<std::string, bool> own_cache;
    if (s.start_commit_hash.empty()) {
        s.own_commits = s.commits_added;
    } else {
        for (const auto& c : s.commits_added) {
            if (is_own_commit(project_dir, s.start_commit_hash, c.hash, own_cache)) {
                s.own_commits.push_back(c);
            }
        }
    }

    std::string engine_pid_path = run_dir + "/engine.pid";
    if (file_exists_local(engine_pid_path)) {
        s.engine_pid = std::atoi(trim(read_file(engine_pid_path)).c_str());
        s.engine_pid_alive = process_alive(s.engine_pid);
        if (s.engine_pid_alive) {
            s.descendant_pids = platform::descendant_pids(s.engine_pid);
        }
    }
}

FailureKind classify_primary(const DiagnosisSignals& signals) {
    if (signals.cherry_pick_conflict) {
        return FailureKind::CherryPickConflict;
    }
    if (signals.prompt_size_kb >= 200) {
        return FailureKind::PromptBlowup;
    }

    if (!signals.role_keywords_found.empty() && !signals.impl_keywords_found.empty()) {
        return FailureKind::RolePromptConflict;
    }

    if (!signals.unresolved_vars.empty()) {
        return FailureKind::VariableCorrupted;
    }

    if (signals.timed_out) {
        bool has_own_commits = !signals.own_commits.empty();
        bool has_any_commits = !signals.commits_added.empty();
        bool has_uncommitted = !signals.files_added_untracked.empty() ||
                               !signals.files_modified_uncommitted.empty();

        if (signals.timeout_kind == "wall_clock" && !has_own_commits) {
            return FailureKind::WallClockWithoutOwnProgress;
        }
        if (signals.timeout_kind == "wall_clock" && has_own_commits) {
            return FailureKind::WallClockWithProgress;
        }
        if (signals.timeout_kind == "idle" || signals.timeout_kind == "wall_clock") {
            if (has_any_commits) return FailureKind::IdleStallWorkCommitted;
            if (has_uncommitted) return FailureKind::IdleStallWorkOnDisk;
            return FailureKind::IdleStallNoWorkSalvageable;
        }
    }

    if (signals.status_status == "FAILURE") {
        return FailureKind::SelfExitError;
    }

    return FailureKind::Unknown;
}

} // namespace

std::string failure_kind_string(FailureKind k) {
    switch (k) {
        case FailureKind::IdleStallNoWorkSalvageable: return "idle_stall_clean";
        case FailureKind::IdleStallWorkOnDisk: return "idle_stall_with_uncommitted_work";
        case FailureKind::IdleStallWorkCommitted: return "idle_stall_with_committed_work";
        case FailureKind::WallClockWithProgress: return "wall_clock_with_progress";
        case FailureKind::WallClockWithoutOwnProgress: return "wall_clock_without_own_progress";
        case FailureKind::SelfExitError: return "self_exit_error";
        case FailureKind::PromptBlowup: return "prompt_blowup";
        case FailureKind::RolePromptConflict: return "role_prompt_conflict";
        case FailureKind::VariableCorrupted: return "variable_corrupted";
        case FailureKind::OrphanedSubprocesses: return "orphaned_subprocesses";
        case FailureKind::CherryPickConflict: return "cherry_pick_conflict";
        case FailureKind::Unknown: return "unknown";
    }
    return "unknown";
}

DiagnosisSignals Diagnose::collect(const std::string& run_dir,
                                   const std::string& failed_node_in) {
    DiagnosisSignals s;

    std::string cp_path = run_dir + "/checkpoint.json";
    if (file_exists_local(cp_path)) {
        try {
            std::ifstream in(cp_path);
            nlohmann::json j;
            in >> j;
            s.current_node = j.value("current_node", "");
            s.graph_file = j.value("graph_file", "");
            if (j.count("completed_nodes")) {
                for (const auto& n : j["completed_nodes"]) {
                    s.completed_nodes.push_back(n.get<std::string>());
                }
            }
            if (j.count("context") && j["context"].is_object() &&
                j["context"].count("needle.last_outcome.status")) {
                s.last_outcome_status =
                    j["context"]["needle.last_outcome.status"].get<std::string>();
            }
        } catch (...) {
        }
    }

    std::string failed_node = failed_node_in.empty() ? s.current_node : failed_node_in;
    fill_stage_signals(s, run_dir, failed_node);
    return s;
}

DiagnosisReport Diagnose::collect_report(const std::string& run_dir,
                                         const std::string& failed_node) {
    DiagnosisReport report;
    report.signals = collect(run_dir, failed_node);
    if (report.signals.failed_node.empty()) {
        report.kind = FailureKind::Unknown;
        return report;
    }

    report.kind = classify(report.signals);

    std::unique_ptr<Graph> g = read_graph_from_file(report.signals.graph_file);
    if (!g) {
        return report;
    }

    const Node* failed = g->find_node(report.signals.failed_node);
    if (!failed || failed->type != NodeType::PARALLEL) {
        return report;
    }

    auto children = g->outgoing_edges(failed->id);
    size_t failed_children = 0;
    for (const Edge* e : children) {
        const std::string child_id = e->to;
        bool completed = std::find(report.signals.completed_nodes.begin(),
                                   report.signals.completed_nodes.end(),
                                   child_id) != report.signals.completed_nodes.end();
        if (completed) continue;

        DiagnosisSignals child_signals = collect(run_dir, child_id);
        if (child_signals.failed_node.empty()) continue;
        bool child_failed = child_signals.status_status == "FAILURE" || child_signals.timed_out;
        if (!child_failed) continue;

        ParallelBranchDiagnosis child;
        child.child_node_id = child_id;
        child.signals = std::move(child_signals);
        child.kind = classify(child.signals);
        report.children.push_back(std::move(child));
        failed_children++;
    }

    if (failed_children == 0) {
        report.children.clear();
    }

    return report;
}

FailureKind Diagnose::classify(const DiagnosisSignals& signals) {
    return classify_primary(signals);
}

std::string Diagnose::render_markdown(const DiagnosisSignals& s, FailureKind kind) {
    DiagnosisReport report;
    report.signals = s;
    report.kind = kind;
    return render_markdown(report);
}

std::string Diagnose::render_markdown(const DiagnosisReport& report) {
    const DiagnosisSignals& s = report.signals;
    FailureKind kind = report.kind;

    std::stringstream out;
    out << "# Needle Troubleshoot — " << s.failed_node << "\n\n";

    out << "## Diagnosis\n";
    out << "- **Failure kind:** " << failure_kind_string(kind) << "\n";
    out << "- **Stage:** " << s.failed_node << "\n";
    out << "- **Status:** " << (s.status_status.empty() ? "(unknown)" : s.status_status) << "\n";
    if (s.timed_out) out << "- **Timeout kind:** " << s.timeout_kind << "\n";
    out << "- **Prompt size:** " << s.prompt_size_kb << " KB\n";
    out << "- **Response size:** " << s.response_md_bytes << " bytes\n";
    out << "- **Commits added during stage:** " << s.commits_added.size() << "\n";
    out << "- **Own commits after stage start:** " << s.own_commits.size() << "\n";

    if (!report.children.empty()) {
        size_t total = 0;
        std::unique_ptr<Graph> g = read_graph_from_file(s.graph_file);
        if (g) {
            if (const Node* failed = g->find_node(s.failed_node)) {
                total = g->outgoing_edges(failed->id).size();
            }
        }
        if (total == 0) total = report.children.size();
        out << "\n## Parallel Branch Failures\n";
        out << "Parallel branch failures: " << report.children.size() << " of " << total << "\n\n";
        for (const auto& child : report.children) {
            out << "- **" << child.child_node_id << "**: " << failure_kind_string(child.kind) << "\n";
        }
    }

    if (!s.descendant_pids.empty()) {
        out << "\n## Orphaned Subprocesses\n";
        out << "- **Engine PID:** " << s.engine_pid << "\n";
        out << "- **Live descendant PIDs:** ";
        for (size_t i = 0; i < s.descendant_pids.size(); ++i) {
            if (i) out << ", ";
            out << s.descendant_pids[i];
        }
        out << "\n";
    }

    out << "\n## Likely root cause\n";
    switch (kind) {
        case FailureKind::RolePromptConflict:
            out << "Prompt contains conflicting role-isolation and implementation instructions.\n";
            break;
        case FailureKind::VariableCorrupted:
            out << "Prompt or logs indicate unresolved $var placeholders.";
            if (!s.unresolved_vars.empty()) {
                out << " Unresolved: ";
                for (size_t i = 0; i < s.unresolved_vars.size(); ++i) {
                    if (i) out << ", ";
                    out << "$var." << s.unresolved_vars[i];
                }
            }
            out << "\n";
            break;
        case FailureKind::WallClockWithoutOwnProgress:
            out << "Wall-clock timeout occurred without commits attributable to this stage.\n";
            break;
        case FailureKind::IdleStallNoWorkSalvageable:
            out << "Agent went silent with no salvageable output.\n";
            break;
        case FailureKind::IdleStallWorkOnDisk:
            out << "Agent stalled after producing uncommitted filesystem changes.\n";
            break;
        case FailureKind::IdleStallWorkCommitted:
            out << "Agent stalled after creating commits.\n";
            break;
        case FailureKind::WallClockWithProgress:
            out << "Stage exceeded wall-clock timeout but made commit progress.\n";
            break;
        case FailureKind::SelfExitError:
            out << "Process exited with failure outside timeout patterns.\n";
            break;
        case FailureKind::PromptBlowup:
            out << "Prompt size is unusually large and likely caused instability.\n";
            break;
        case FailureKind::OrphanedSubprocesses:
            out << "Live descendants are still attached to the engine process.\n";
            break;
        case FailureKind::CherryPickConflict:
            out << "Fan-in cherry-pick conflict requires manual operator merge.\n";
            break;
        case FailureKind::Unknown:
            out << "Pattern matcher could not classify this failure.\n";
            break;
    }

    out << "\n## Proposed actions (v2: diagnose only)\n";
    out << "No automatic recovery steps are applied in this command.\n";

    return out.str();
}

} // namespace needle
