#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler.h"
#include "needle/engine/subgraph_executor.h"
#include "needle/engine/subgraph_topology.h"
#include <memory>
#include <set>

namespace needle {

class ManagerLoopHandler : public Handler {
public:
    explicit ManagerLoopHandler(std::shared_ptr<SubgraphExecutor> executor)
        : executor_(std::move(executor)) {}

    std::string type_name() const override { return "manager_loop"; }

    Result<Outcome> execute(const Node& node, Context& ctx,
                            const ExecutionContext& exec_ctx) override {
        // Get max iterations
        int max_iterations = 10;
        Maybe<int> mi = node.attrs.get_int("max_iterations");
        if (mi.has_value()) {
            max_iterations = *mi;
        }

        // Get outgoing edges to find the subgraph start
        auto edges = exec_ctx.graph.outgoing_edges(node.id);
        if (edges.empty()) {
            return Result<Outcome>::failure("manager_loop has no outgoing edges: " + node.id);
        }

        std::string subgraph_start = edges[0]->to;

        // Find the end of the subgraph: walk forward to find a node whose outgoing edge
        // targets this manager_loop node again, or that has no further edges within the loop.
        std::string subgraph_end;
        {
            const Node* n = exec_ctx.graph.find_node(subgraph_start);
            std::vector<std::string> visited;
            while (n) {
                visited.push_back(n->id);
                auto next_edges = exec_ctx.graph.outgoing_edges(n->id);
                bool found_loop_back = false;
                for (const auto* e : next_edges) {
                    if (e->to == node.id) {
                        subgraph_end = n->id;
                        found_loop_back = true;
                        break;
                    }
                }
                if (found_loop_back) break;

                if (next_edges.empty()) {
                    subgraph_end = n->id;
                    break;
                }

                // Move to next
                n = exec_ctx.graph.find_node(next_edges[0]->to);

                // Prevent infinite traversal
                bool cycle = false;
                for (const auto& v : visited) {
                    if (n && n->id == v) { cycle = true; break; }
                }
                if (cycle) {
                    subgraph_end = visited.back();
                    break;
                }
            }
        }

        if (subgraph_end.empty()) {
            return Result<Outcome>::failure("could not find subgraph end for manager_loop: " + node.id);
        }

        // M12: Compute the set of nodes in this loop's body using BFS.
        // Only goal gates within this set count toward loop termination.
        std::set<std::string> loop_body = SubgraphTopology::collect_loop_body(
            exec_ctx.graph, node.id, subgraph_start);

        // Loop
        for (int i = 0; i < max_iterations; ++i) {
            auto result = executor_->execute_subgraph(subgraph_start, subgraph_end, ctx, exec_ctx);
            if (!result.ok()) {
                return result;
            }

            Outcome sub_outcome = result.value();

            // M12: Check if goal gates WITHIN THE LOOP BODY are satisfied.
            // Goal gates outside the managed subgraph are ignored.
            bool all_goals_satisfied = true;
            for (const auto& n : exec_ctx.graph.nodes()) {
                if (!n.goal_gate()) continue;
                // Only check nodes that are part of this loop's body
                if (loop_body.find(n.id) == loop_body.end()) continue;
                std::string goal_key = "goal." + n.id;
                if (!ctx.has(goal_key) || ctx.get(goal_key) != "satisfied") {
                    all_goals_satisfied = false;
                    break;
                }
            }

            if (all_goals_satisfied || sub_outcome.status == StageStatus::SUCCESS) {
                if (all_goals_satisfied) {
                    Outcome outcome;
                    outcome.status = StageStatus::SUCCESS;
                    outcome.output = "manager loop completed after " + std::to_string(i + 1) + " iterations";
                    outcome.context_updates["manager_loop." + node.id + ".iterations"] = std::to_string(i + 1);
                    return Result<Outcome>::success(std::move(outcome));
                }
            }

            if (sub_outcome.status == StageStatus::FAILURE) {
                return Result<Outcome>::success(std::move(sub_outcome));
            }
        }

        Outcome outcome;
        outcome.status = StageStatus::FAILURE;
        outcome.output = "manager loop max iterations reached: " + std::to_string(max_iterations);
        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<SubgraphExecutor> executor_;
};

std::shared_ptr<Handler> make_manager_loop_handler(std::shared_ptr<SubgraphExecutor> executor) {
    return std::make_shared<ManagerLoopHandler>(std::move(executor));
}

} // namespace needle
