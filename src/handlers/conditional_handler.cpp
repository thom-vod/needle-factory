#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler.h"
#include "needle/model/condition.h"
#include "needle/parser/condition_parser.h"

namespace needle {

class ConditionalHandler : public Handler {
public:
    std::string type_name() const override { return "conditional"; }

    Result<Outcome> execute(const Node& node, Context& ctx,
                            const ExecutionContext& exec_ctx) override {
        Outcome outcome;
        outcome.status = StageStatus::SUCCESS;

        // Build eval outcome from actual parent outcome (M3 fix)
        Outcome eval_outcome;
        std::string prev_status = ctx.get("needle.last_outcome.status");
        if (prev_status == "FAILURE") {
            eval_outcome.status = StageStatus::FAILURE;
        } else if (prev_status == "RETRY") {
            eval_outcome.status = StageStatus::RETRY;
        } else if (prev_status == "PARTIAL_SUCCESS") {
            eval_outcome.status = StageStatus::PARTIAL_SUCCESS;
        } else if (prev_status == "SKIP") {
            eval_outcome.status = StageStatus::SKIP;
        } else {
            eval_outcome.status = StageStatus::SUCCESS; // default for backward compat
        }

        // Get outgoing edges
        auto edges = exec_ctx.graph.outgoing_edges(node.id);

        // Evaluate conditions on each edge
        for (const auto* edge : edges) {
            std::string cond_str = edge->condition();
            if (cond_str.empty()) {
                // No condition means always-match — use as default
                if (outcome.preferred_label.empty()) {
                    outcome.preferred_label = edge->label();
                }
                continue;
            }

            auto parse_result = ConditionParser::parse(cond_str);
            if (!parse_result.ok()) {
                continue; // Skip unparseable conditions
            }

            ConditionExpr expr = parse_result.value();
            if (expr.evaluate(eval_outcome, ctx)) {
                outcome.preferred_label = edge->label();
                break; // First matching condition wins
            }
        }

        return Result<Outcome>::success(std::move(outcome));
    }
};

std::shared_ptr<Handler> make_conditional_handler() {
    return std::make_shared<ConditionalHandler>();
}

} // namespace needle
