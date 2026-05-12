#include "needle/engine/remediation_plan.h"
#include "needle/engine/remediation_executor.h"

#include <algorithm>

#include "needle/engine/checkpoint_manager.h"
#include "needle/engine/stage_advancer.h"
#include "needle/platform/platform.h"

namespace needle {

static Result<void> stage_retry_like(const std::string& run_dir, const std::string& node_id) {
    JsonCheckpointWriter writer;
    auto loaded = writer.load(run_dir + "/checkpoint.json");
    if (!loaded.ok()) return Result<void>::failure(loaded.error());
    Checkpoint cp = loaded.value();

    cp.current_node = node_id;
    cp.completed_nodes.erase(std::remove(cp.completed_nodes.begin(), cp.completed_nodes.end(), node_id),
                             cp.completed_nodes.end());
    cp.retry_counters.clear();

    return writer.save(cp, run_dir + "/checkpoint.json");
}

RemediationExecutionResult execute_remediation_plan(const RemediationPlan& plan,
                                                    Graph& graph,
                                                    const std::string& run_dir) {
    RemediationExecutionResult out;

    auto apply_primary = [&](RemediationPlan::Type type) {
        if (type == RemediationPlan::Type::MarkSuccessAdvance) {
            auto r1 = StageAdvancer::mark(run_dir, plan.node_id, true,
                                          "auto-troubleshoot recovery: " + plan.reason);
            if (!r1.ok()) {
                out.escalated = true;
                out.message = r1.error();
                return;
            }
            out.actions.push_back("needle stage mark " + plan.node_id + " success");
            if (!plan.next_node.empty()) {
                auto r2 = StageAdvancer::advance(run_dir, plan.next_node);
                if (!r2.ok()) {
                    out.escalated = true;
                    out.message = r2.error();
                    return;
                }
                out.actions.push_back("needle stage advance --to " + plan.next_node);
            }
            out.resumed = true;
            return;
        }

        if (type == RemediationPlan::Type::ResetAndRetry) {
            auto r = stage_retry_like(run_dir, plan.node_id);
            if (!r.ok()) {
                out.escalated = true;
                out.message = r.error();
                return;
            }
            out.actions.push_back("needle stage retry " + plan.node_id);
            out.resumed = true;
            return;
        }

        if (type == RemediationPlan::Type::ResetWithLowerFidelity) {
            Node* n = graph.mutable_node(plan.node_id);
            if (n && !plan.fidelity_override.empty()) {
                n->attrs.set("fidelity", plan.fidelity_override);
            }
            auto r = stage_retry_like(run_dir, plan.node_id);
            if (!r.ok()) {
                out.escalated = true;
                out.message = r.error();
                return;
            }
            out.actions.push_back("needle stage retry " + plan.node_id + " (fidelity=" + plan.fidelity_override + ")");
            out.resumed = true;
            return;
        }

        out.escalated = true;
        out.message = "plan requires escalation";
    };

    if (plan.type == RemediationPlan::Type::KillOrphansFirst) {
        for (int pid : plan.orphan_pids) {
            platform::kill_process(pid);
        }
        out.actions.push_back("kill orphan pids");
        apply_primary(plan.primary_after_orphan);
        return out;
    }

    apply_primary(plan.type);
    return out;
}

} // namespace needle
