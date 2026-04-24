#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/backend/backend.h"
#include <memory>
#include <fstream>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace needle {

class CodergenHandler : public HandlerBase {
public:
    explicit CodergenHandler(std::shared_ptr<Backend> backend)
        : backend_(std::move(backend)) {}

    std::string type_name() const override { return "codergen"; }

    Result<Outcome> do_execute(const Node& node, Context& ctx,
                               const ExecutionContext& exec_ctx) override {
        // Build stage directory
        std::string stage_dir;
        if (!exec_ctx.logs_root.empty()) {
            stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
        }

        // Clear stale files from previous runs before executing
        if (!stage_dir.empty()) {
            std::remove((stage_dir + "/response.md").c_str());
            std::remove((stage_dir + "/status.json").c_str());
        }

        auto result = backend_->execute(node, ctx, stage_dir);
        if (!result.ok()) {
            return result;
        }

        Outcome outcome = result.value();

        // Note: status.json override was removed. Agents running with full file
        // access frequently write to the stage directory inadvertently (and even
        // commit .needle/stages/*/status.json), causing false FAILURE statuses.
        // The backend's exit code is the authoritative success/failure signal.

        // Store output in context so downstream nodes can reference it
        // via $context.codergen.{node_id}.output
        if (!outcome.output.empty()) {
            outcome.context_updates["codergen." + node.id + ".output"] = outcome.output;
        }

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<Backend> backend_;
};

std::shared_ptr<Handler> make_codergen_handler(std::shared_ptr<Backend> backend) {
    return std::make_shared<CodergenHandler>(std::move(backend));
}

} // namespace needle
