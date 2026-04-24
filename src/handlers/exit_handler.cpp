#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler.h"

namespace needle {

class ExitHandler : public Handler {
public:
    std::string type_name() const override { return "exit"; }

    Result<Outcome> execute(const Node& node, Context& ctx,
                            const ExecutionContext& exec_ctx) override {
        (void)node;
        Outcome outcome;

        // Check goal gates
        std::vector<std::string> unsatisfied;
        for (const auto& n : exec_ctx.graph.nodes()) {
            if (n.goal_gate()) {
                std::string goal_key = "goal." + n.id;
                if (!ctx.has(goal_key) || ctx.get(goal_key) != "satisfied") {
                    unsatisfied.push_back(n.id);
                }
            }
        }

        if (!unsatisfied.empty()) {
            outcome.status = StageStatus::RETRY;
            outcome.suggested_next = unsatisfied;
            outcome.output = "goal gates unsatisfied";
        } else {
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "all goal gates satisfied";
        }

        return Result<Outcome>::success(std::move(outcome));
    }
};

std::shared_ptr<Handler> make_exit_handler() {
    return std::make_shared<ExitHandler>();
}

} // namespace needle
