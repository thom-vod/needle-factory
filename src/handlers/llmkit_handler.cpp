#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/backend/backend.h"
#include <memory>

namespace needle {

class LLMKitHandler : public HandlerBase {
public:
    explicit LLMKitHandler(std::shared_ptr<Backend> backend)
        : backend_(std::move(backend)) {}

    std::string type_name() const override { return "llmkit"; }

    Result<Outcome> do_execute(const Node& node, Context& ctx,
                               const ExecutionContext& exec_ctx) override {
        std::string stage_dir;
        if (!exec_ctx.logs_root.empty()) {
            stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
        }
        return backend_->execute(node, ctx, stage_dir);
    }

private:
    std::shared_ptr<Backend> backend_;
};

std::shared_ptr<Handler> make_llmkit_handler(std::shared_ptr<Backend> backend) {
    return std::make_shared<LLMKitHandler>(std::move(backend));
}

} // namespace needle
