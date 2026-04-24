#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler.h"
#include "needle/event/event.h"

namespace needle {

class StartHandler : public Handler {
public:
    std::string type_name() const override { return "start"; }

    Result<Outcome> execute(const Node& node, Context& ctx,
                            const ExecutionContext& /*exec_ctx*/) override {
        (void)node;
        (void)ctx;
        Outcome outcome;
        outcome.status = StageStatus::SUCCESS;
        outcome.context_updates["pipeline.start_time"] = utc_timestamp_now();
        return Result<Outcome>::success(std::move(outcome));
    }
};

std::shared_ptr<Handler> make_start_handler() {
    return std::make_shared<StartHandler>();
}

} // namespace needle
