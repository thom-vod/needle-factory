#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler.h"
#include "needle/backend/process_runner.h"
#include "needle/engine/subgraph_executor.h"
#include "needle/engine/retry_controller.h"
#include "needle/event/worktree_ready_event.h"
#include "needle/util/logger.h"
#include "needle/worktree/manager.h"
#include "needle/worktree/strategy.h"
#include <memory>
#include <thread>
#include <mutex>
#include <vector>
#include <set>
#include <queue>
#include <algorithm>
#include <cstdio>

namespace needle {

namespace {

std::string basename_of(const std::string& path) {
    if (path.empty()) return path;
    size_t end = path.size();
    while (end > 1 && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
    size_t slash = path.find_last_of("/\\", end - 1);
    if (slash == std::string::npos) return path.substr(0, end);
    return path.substr(slash + 1, end - slash - 1);
}

std::string parent_of(const std::string& path) {
    if (path.empty()) return ".";
    size_t end = path.size();
    while (end > 1 && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
    size_t slash = path.find_last_of("/\\", end - 1);
    if (slash == std::string::npos) return ".";
    if (slash == 0) return path.substr(0, 1);
    return path.substr(0, slash);
}

std::string launch_head_commit(const std::string& repo) {
    // Run git via the process runner (no shell). The previous
    // `git -C '<repo>' rev-parse HEAD 2>/dev/null` popen string was POSIX-only:
    // on Windows it routed through cmd.exe, which passed the single quotes
    // literally to git and so always failed, leaving the launch commit empty.
    NativeProcessRunner runner;
    auto r = runner.run("git", {"-C", repo, "rev-parse", "HEAD"}, repo, 10000);
    if (!r.ok() || r.value().exit_code != 0) return "";
    std::string out = r.value().stdout_output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// M15: BFS-based fan-in discovery. Find the common FAN_IN node reachable from all branch starts.
// Handles internal conditional edges, multiple outgoing edges, and branches converging at different depths.
std::string find_common_fan_in_bfs(const Graph& graph, const std::vector<std::string>& branch_starts) {
    if (branch_starts.empty()) return "";

    // For each branch, collect all reachable FAN_IN nodes via BFS
    std::vector<std::set<std::string>> per_branch_fan_ins;

    for (const auto& start : branch_starts) {
        std::set<std::string> fan_ins;
        std::set<std::string> visited;
        std::queue<std::string> bfs_queue;

        bfs_queue.push(start);
        visited.insert(start);

        while (!bfs_queue.empty()) {
            std::string node_id = bfs_queue.front();
            bfs_queue.pop();

            const Node* node = graph.find_node(node_id);
            if (!node) continue;

            if (node->type == NodeType::FAN_IN) {
                fan_ins.insert(node_id);
                // Don't traverse past fan-in nodes
                continue;
            }

            auto edges = graph.outgoing_edges(node_id);
            for (const auto* e : edges) {
                if (visited.find(e->to) == visited.end()) {
                    visited.insert(e->to);
                    bfs_queue.push(e->to);
                }
            }
        }
        per_branch_fan_ins.push_back(std::move(fan_ins));
    }

    if (per_branch_fan_ins.empty()) return "";

    // Intersect: find fan-in nodes reachable from ALL branches
    std::set<std::string> common = per_branch_fan_ins[0];
    for (size_t i = 1; i < per_branch_fan_ins.size(); ++i) {
        std::set<std::string> intersection;
        for (const auto& id : common) {
            if (per_branch_fan_ins[i].count(id)) {
                intersection.insert(id);
            }
        }
        common = std::move(intersection);
    }

    if (common.empty()) return "";
    if (common.size() == 1) return *common.begin();

    // Multiple common fan-ins: pick the nearest (shortest BFS distance from any branch)
    // with lexical tiebreak
    int best_dist = 999999;
    std::string best_id;
    for (const auto& fan_in_id : common) {
        for (const auto& start : branch_starts) {
            // BFS to find distance
            std::queue<std::pair<std::string, int>> q;
            std::set<std::string> vis;
            q.push({start, 0});
            vis.insert(start);
            while (!q.empty()) {
                auto cur = q.front(); q.pop();
                if (cur.first == fan_in_id) {
                    if (cur.second < best_dist ||
                        (cur.second == best_dist && fan_in_id < best_id)) {
                        best_dist = cur.second;
                        best_id = fan_in_id;
                    }
                    break;
                }
                auto edges = graph.outgoing_edges(cur.first);
                for (const auto* e : edges) {
                    if (vis.find(e->to) == vis.end()) {
                        vis.insert(e->to);
                        q.push({e->to, cur.second + 1});
                    }
                }
            }
        }
    }
    return best_id;
}

} // anonymous namespace

class ParallelHandler : public Handler {
public:
    ParallelHandler(std::shared_ptr<SubgraphExecutor> executor, const WorktreeConfig& worktree_cfg)
        : executor_(std::move(executor)), worktree_cfg_(worktree_cfg) {}

    std::string type_name() const override { return "parallel"; }

    Result<Outcome> execute(const Node& node, Context& ctx,
                            const ExecutionContext& exec_ctx) override {
        auto edges = exec_ctx.graph.outgoing_edges(node.id);
        if (edges.empty()) {
            Outcome o;
            o.status = StageStatus::SUCCESS;
            return Result<Outcome>::success(std::move(o));
        }

        // M15: BFS-based fan-in discovery
        std::vector<std::string> branch_starts;
        for (const auto* edge : edges) {
            branch_starts.push_back(edge->to);
        }
        std::string fan_in_id = find_common_fan_in_bfs(exec_ctx.graph, branch_starts);

        if (fan_in_id.empty()) {
            return Result<Outcome>::failure("no fan-in node found for parallel: " + node.id);
        }

        // Get join policy
        std::string join_policy = node.attrs.get("join_policy", "wait_all");
        NEEDLE_LOG_INFO("parallel", "node %s: join_policy=%s join_threshold=%s",
                        node.id.c_str(), join_policy.c_str(),
                        node.attrs.get("join_threshold", "(not set)").c_str());

        // Spawn threads for each branch
        struct BranchResult {
            std::string branch_target;
            Result<Outcome> result;
            Context branch_ctx;

            BranchResult() : result(Result<Outcome>::failure("not started")) {}
        };

        std::vector<std::shared_ptr<BranchResult>> results;
        results.reserve(edges.size());
        for (size_t i = 0; i < edges.size(); ++i) {
            results.push_back(std::make_shared<BranchResult>());
            results.back()->branch_target = edges[i]->to;
        }

        std::mutex mutex;
        bool first_success = false;
        (void)first_success;

        // Check which branches already succeeded in a previous run (for resume)
        std::vector<bool> already_done(edges.size(), false);
        for (size_t i = 0; i < edges.size(); ++i) {
            std::string prev_status = ctx.get("parallel." + edges[i]->to + ".status");
            if (prev_status == "SUCCESS") {
                already_done[i] = true;
                // Carry forward the previous result without re-executing
                auto br = results[i];
                Outcome prev_outcome;
                prev_outcome.status = StageStatus::SUCCESS;
                prev_outcome.output = ctx.get("parallel." + edges[i]->to + ".output");
                br->result = Result<Outcome>::success(std::move(prev_outcome));
            }
        }

        std::vector<std::thread> threads;
        std::map<std::string, std::string> pre_context_updates;
        bool pre_any_failure = false;
        if (worktree_cfg_.strategy == WorktreeStrategy::Auto) {
            std::string launch_commit = ctx.get("needle.launch_commit");
            if (launch_commit.empty()) {
                launch_commit = launch_head_commit(exec_ctx.project_dir);
                if (!launch_commit.empty()) {
                    pre_context_updates["needle.launch_commit"] = launch_commit;
                }
            }
        }
        for (size_t i = 0; i < edges.size(); ++i) {
            if (already_done[i]) {
                NEEDLE_LOG_INFO("parallel", "skipping branch %s (already succeeded in previous run)",
                                edges[i]->to.c_str());
                continue;
            }

            auto br = results[i];
            Context branch_ctx = ctx.clone();
            std::string start_id = edges[i]->to;
            std::string end_id = fan_in_id;

            bool skip_branch = false;
            if (worktree_cfg_.strategy == WorktreeStrategy::Auto) {
                std::map<std::string, std::string> params = {
                    {"run_id", ctx.get("needle.run_id")},
                    {"repo_basename", basename_of(exec_ctx.project_dir)},
                    {"branch_id", start_id},
                };

                auto branch_path_name = interpolate_template(
                    "${repo_basename}-wt-${run_id}-${branch_id}", params);
                auto branch_name = interpolate_template(
                    worktree_cfg_.branch + "/${branch_id}", params);
                if (!branch_path_name.ok() || !branch_name.ok()) {
                    skip_branch = true;
                    pre_context_updates["parallel." + start_id + ".status"] = "FAILURE";
                    pre_context_updates["parallel." + start_id + ".output"] = "worktree_setup_failed";
                    pre_context_updates["parallel." + start_id + ".annotation"] = "worktree_setup_failed";
                    pre_any_failure = true;
                } else {
                    WorktreeConfig branch_cfg = worktree_cfg_;
                    branch_cfg.path = parent_of(exec_ctx.project_dir) + "/" + branch_path_name.value();
                    branch_cfg.branch = branch_name.value();
                    auto ready = WorktreeManager::ensure_ready(exec_ctx.project_dir, branch_cfg);
                    if (!ready.ok()) {
                        skip_branch = true;
                        pre_context_updates["parallel." + start_id + ".status"] = "FAILURE";
                        pre_context_updates["parallel." + start_id + ".output"] = "worktree_setup_failed: " + ready.error();
                        pre_context_updates["parallel." + start_id + ".annotation"] = "worktree_setup_failed";
                        pre_any_failure = true;
                    } else {
                        branch_ctx.set("needle.branch.cwd", ready.value().path);
                        branch_ctx.set("needle.branch.worktree", ready.value().path);
                        branch_ctx.set("needle.branch.git_branch", ready.value().branch);
                        pre_context_updates["needle.branch_worktree." + start_id] = ready.value().path;

                        WorktreeReadyEvent ev;
                        ev.branch_id = start_id;
                        ev.path = ready.value().path;
                        ev.branch = ready.value().branch;
                        ev.created_now = ready.value().created_now;

                        PipelineEvent pe;
                        pe.type = EventType::STAGE_WARNING;
                        pe.timestamp = utc_timestamp_now();
                        pe.node_id = node.id;
                        pe.message = "worktree_ready";
                        pe.data["branch_id"] = ev.branch_id;
                        pe.data["path"] = ev.path;
                        pe.data["branch"] = ev.branch;
                        pe.data["created_now"] = ev.created_now;
                        exec_ctx.event_bus.emit(pe);
                    }
                }
            }
            if (skip_branch) {
                continue;
            }

            // M7: Create a per-branch RetryController copy
            // Branch retry counts are discarded at fan-in (no merge back)
            // inclusive_end=false — stop before the fan-in so it runs once in
            // the parent context with parallel.* state fully populated.
            threads.push_back(std::thread([this, br, branch_ctx, start_id, end_id, &exec_ctx]() mutable {
                br->branch_ctx = branch_ctx;
                RetryController branch_rc;  // fresh per-branch copy
                br->result = executor_->execute_subgraph(start_id, end_id, br->branch_ctx, exec_ctx, &branch_rc, /*inclusive_end=*/false);
            }));
        }

        // Wait for all threads
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        // Merge contexts with namespaced keys
        Outcome outcome;
        outcome.status = StageStatus::SUCCESS;
        outcome.context_updates.insert(pre_context_updates.begin(), pre_context_updates.end());

        bool any_failure = pre_any_failure;
        for (const auto& br : results) {
            if (!br->result.ok()) {
                any_failure = true;
                NEEDLE_LOG_ERROR("parallel", "branch %s: Result::failure: %s",
                                 br->branch_target.c_str(), br->result.error().c_str());
                continue;
            }
            NEEDLE_LOG_INFO("parallel", "branch %s: status=%s output_len=%zu",
                            br->branch_target.c_str(),
                            to_string(br->result.value().status).c_str(),
                            br->result.value().output.size());

            Outcome branch_outcome = br->result.value();
            if (branch_outcome.status == StageStatus::FAILURE) {
                any_failure = true;
            }

            // Merge with namespace: branch_target.key
            for (const auto& kv : branch_outcome.context_updates) {
                outcome.context_updates[br->branch_target + "." + kv.first] = kv.second;
            }

            // Store branch status and output
            outcome.context_updates["parallel." + br->branch_target + ".status"] =
                to_string(branch_outcome.status);
            outcome.context_updates["parallel." + br->branch_target + ".output"] =
                branch_outcome.output;
        }

        if (join_policy == "first_success") {
            bool any_success_found = false;
            for (const auto& br : results) {
                if (br->result.ok() && br->result.value().status == StageStatus::SUCCESS) {
                    any_success_found = true;
                    break;
                }
            }
            if (!any_success_found) {
                outcome.status = StageStatus::FAILURE;
            }
        } else if (join_policy == "threshold") {
            int threshold = 2;
            Maybe<int> t = node.attrs.get_int("join_threshold");
            if (t.has_value()) {
                threshold = *t;
            }
            int success_count = 0;
            for (const auto& br : results) {
                if (br->result.ok() &&
                    (br->result.value().status == StageStatus::SUCCESS ||
                     br->result.value().status == StageStatus::PARTIAL_SUCCESS)) {
                    ++success_count;
                }
            }
            NEEDLE_LOG_INFO("parallel", "threshold policy: %d/%zu succeeded (need %d)",
                            success_count, results.size(), threshold);
            if (success_count < threshold) {
                outcome.status = StageStatus::FAILURE;
            }
        } else {
            // wait_all: fail if any branch failed
            if (any_failure) {
                outcome.status = StageStatus::FAILURE;
            }
        }

        // M13: The fan-in node was already executed inside each branch's subgraph
        // (inclusive-end semantics). Store the fan-in target so the main engine loop
        // can skip it and advance to the node after fan-in.
        outcome.context_updates["parallel.fan_in_target"] = fan_in_id;

        // Store metadata for fan-in
        outcome.context_updates["parallel.branch_count"] = std::to_string(edges.size());
        outcome.context_updates["parallel.join_policy"] = join_policy;

        // Store branch list (comma-separated) for fan-in to enumerate
        std::string branch_list;
        for (const auto& br : results) {
            if (!branch_list.empty()) branch_list += ",";
            branch_list += br->branch_target;
        }
        outcome.context_updates["parallel.branches"] = branch_list;

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<SubgraphExecutor> executor_;
    WorktreeConfig worktree_cfg_;
};

std::shared_ptr<Handler> make_parallel_handler(std::shared_ptr<SubgraphExecutor> executor,
                                               const WorktreeConfig& worktree_cfg) {
    return std::make_shared<ParallelHandler>(std::move(executor), worktree_cfg);
}

} // namespace needle
