#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler.h"
#include "needle/backend/backend.h"
#include "needle/engine/fan_in_merger.h"
#include "needle/config/needle_config.h"
#include "needle/worktree/strategy.h"

#include <sstream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

namespace needle {

class FanInHandler : public Handler {
public:
    explicit FanInHandler(std::shared_ptr<Backend> backend = nullptr)
        : backend_(std::move(backend)) {}

    std::string type_name() const override { return "fan_in"; }

    Result<Outcome> execute(const Node& node, Context& ctx,
                            const ExecutionContext& exec_ctx) override {
        // Short-circuit if called before the parallel handler has populated
        // parallel.* state (e.g. from inside a branch's subgraph). The real
        // merge happens in the parent context after parallel returns.
        std::string branches_str = ctx.get("parallel.branches");
        if (branches_str.empty()) {
            Outcome outcome;
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "fan-in deferred (parallel state not ready)";
            return Result<Outcome>::success(std::move(outcome));
        }

        std::string join_policy = ctx.get("parallel.join_policy");
        auto branches = parse_branches(ctx);

        bool has_worktree = false;
        for (const auto& b : branches) {
            if (!ctx.get("needle.branch_worktree." + b).empty()) {
                has_worktree = true;
                break;
            }
        }
        if (has_worktree) {
            WorktreeConfig wt_cfg;
            wt_cfg.strategy = worktree_strategy_from_string(
                NeedleConfig::global().get_string("worktree.strategy", "", "off"));
            wt_cfg.cleanup = NeedleConfig::global().get_string("worktree.cleanup", "", "keep");
            auto merged = FanInMerger::merge(exec_ctx.project_dir, ctx.get("needle.launch_commit"),
                                             branches, ctx, wt_cfg);
            if (!merged.ok()) return Result<Outcome>::failure(merged.error());
            if (!merged.value().ok) {
                Outcome fail;
                fail.status = StageStatus::FAILURE;
                fail.output = "cherry-pick conflict at " + merged.value().conflict.branch_that_conflicted;
                nlohmann::json block;
                block["branch_that_conflicted"] = merged.value().conflict.branch_that_conflicted;
                block["branches_already_applied"] = merged.value().conflict.branches_already_applied;
                block["branches_pending"] = merged.value().conflict.branches_pending;
                block["conflicting_files"] = merged.value().conflict.conflicting_files;
                block["git_status"] = merged.value().conflict.git_status;
                fail.context_updates["fan_in." + node.id + ".cherry_pick_conflict"] = block.dump();
                return Result<Outcome>::success(std::move(fail));
            }
        }

        if (join_policy == "consensus" && backend_) {
            return execute_consensus(node, ctx, exec_ctx);
        }

        // Default path: if the node has its own prompt, run it via the backend
        // with branch outputs appended so wait_all / threshold / first_success
        // merges with a custom prompt actually produce content. Without a
        // prompt or backend, fall back to the passthrough marker.
        if (!node.prompt().empty() && backend_) {
            return execute_prompted_merge(node, ctx, exec_ctx, join_policy);
        }

        Outcome outcome;
        outcome.status = StageStatus::SUCCESS;
        outcome.output = "fan-in merge complete";
        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::vector<std::string> parse_branches(const Context& ctx) {
        std::vector<std::string> branches;
        std::istringstream iss(ctx.get("parallel.branches"));
        std::string branch;
        while (std::getline(iss, branch, ',')) {
            if (!branch.empty()) branches.push_back(branch);
        }
        return branches;
    }

    std::string build_branch_outputs_section(const std::vector<std::string>& branches,
                                              const Context& ctx) {
        std::ostringstream s;
        s << "## Branch Outputs\n\n";
        for (const auto& branch : branches) {
            std::string output = ctx.get("parallel." + branch + ".output");
            std::string status = ctx.get("parallel." + branch + ".status");
            s << "### Branch: " << branch
              << " (status: " << status << ")\n"
              << output << "\n\n";
        }
        return s.str();
    }

    void copy_model_attrs(const Node& from, Node& to) {
        std::string provider = from.attrs.get("llm_provider");
        if (!provider.empty()) to.attrs.set("llm_provider", provider);
        std::string model = from.attrs.get("llm_model");
        if (!model.empty()) to.attrs.set("llm_model", model);
        std::string agent = from.attrs.get("agent");
        if (!agent.empty()) to.attrs.set("agent", agent);
    }

    Result<Outcome> execute_consensus(const Node& node, Context& ctx,
                                       const ExecutionContext& exec_ctx) {
        auto branches = parse_branches(ctx);

        std::string fan_in_prompt = node.prompt();
        if (fan_in_prompt.empty()) {
            fan_in_prompt = "Evaluate the following branch outputs and produce a consensus result. "
                            "Identify the best elements from each branch, resolve any conflicts, "
                            "and synthesize a unified output.";
        }

        std::string full_prompt = fan_in_prompt + "\n\n" +
                                   build_branch_outputs_section(branches, ctx);

        Node consensus_node;
        consensus_node.id = node.id;  // reuse real id so stage dir aligns
        consensus_node.type = NodeType::FAN_IN;
        consensus_node.attrs.set("prompt", full_prompt);
        copy_model_attrs(node, consensus_node);

        std::string stage_dir;
        if (!exec_ctx.logs_root.empty()) {
            stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
        }

        auto result = backend_->execute(consensus_node, ctx, stage_dir);
        if (!result.ok()) return result;

        Outcome outcome = result.value();
        // Publish under both keys so old templates using parallel.consensus.result
        // and standard $context.codergen.<fan_in_id>.output both work.
        outcome.context_updates["parallel.consensus.result"] = outcome.output;
        outcome.context_updates["codergen." + node.id + ".output"] = outcome.output;
        return Result<Outcome>::success(std::move(outcome));
    }

    Result<Outcome> execute_prompted_merge(const Node& node, Context& ctx,
                                            const ExecutionContext& exec_ctx,
                                            const std::string& join_policy) {
        auto branches = parse_branches(ctx);
        std::string full_prompt = node.prompt() + "\n\n" +
                                   build_branch_outputs_section(branches, ctx);

        Node merge_node;
        merge_node.id = node.id;
        merge_node.type = NodeType::FAN_IN;
        merge_node.attrs.set("prompt", full_prompt);
        copy_model_attrs(node, merge_node);

        std::string stage_dir;
        if (!exec_ctx.logs_root.empty()) {
            stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
        }

        auto result = backend_->execute(merge_node, ctx, stage_dir);
        if (!result.ok()) return result;

        Outcome outcome = result.value();
        outcome.context_updates["codergen." + node.id + ".output"] = outcome.output;
        (void)join_policy;  // reserved for future per-policy shaping
        return Result<Outcome>::success(std::move(outcome));
    }

    std::shared_ptr<Backend> backend_;
};

std::shared_ptr<Handler> make_fan_in_handler(std::shared_ptr<Backend> backend) {
    return std::make_shared<FanInHandler>(std::move(backend));
}

} // namespace needle
