#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/handlers/interactive_session.h"
#include "needle/backend/backend.h"
#include "needle/event/event.h"
#include "needle/util/context_expand.h"
#include "needle/util/fs_helpers.h"
#include "needle/util/logger.h"

#include <memory>
#include <fstream>
#include "needle/platform/platform.h"

namespace needle {

class InteractiveHandler : public HandlerBase {
public:
    InteractiveHandler(std::shared_ptr<Backend> backend,
                       std::shared_ptr<InteractiveSession> session)
        : backend_(std::move(backend))
        , session_(std::move(session)) {}

    std::string type_name() const override { return "interactive"; }

    Result<Outcome> do_execute(const Node& node, Context& ctx,
                               const ExecutionContext& exec_ctx) override {
        std::string mode = node.attrs.get("mode", "interactive");

        // If mode is autonomous, delegate to the backend like codergen
        if (mode == "autonomous" && backend_) {
            std::string stage_dir;
            if (!exec_ctx.logs_root.empty()) {
                stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
            }
            return backend_->execute(node, ctx, stage_dir);
        }

        // Interactive mode: populate session and block
        std::string prompt = node.prompt().empty() ? node.label() : node.prompt();
        // Resolve $context.* refs against the live pipeline context so the
        // user sees upstream outputs inlined rather than raw placeholders.
        prompt = expand_context_refs(prompt, ctx);

        // Build context summary from pipeline state
        std::string context_summary;
        std::string seed = ctx.get("var.seed");
        if (!seed.empty()) {
            context_summary += "## Project Idea\n\n" + seed + "\n\n";
        }
        std::string consensus = ctx.get("parallel.consensus.result");
        if (!consensus.empty()) {
            context_summary += "## Prior Consensus Result\n\n" + consensus + "\n\n";
        }

        // Build pipeline context: where we are in the graph
        std::string pipeline_context;
        std::string previous_node_id;
        {
            const auto& graph = exec_ctx.graph;
            std::string pipeline_name = graph.graph_attrs().get("label", graph.name());

            // Find next step(s)
            auto out_edges = graph.outgoing_edges(node.id);
            std::string next_steps;
            for (const auto* e : out_edges) {
                const Node* next = graph.find_node(e->to);
                if (next) {
                    if (!next_steps.empty()) next_steps += ", ";
                    next_steps += next->label();
                }
            }

            // Find previous step(s)
            auto in_edges = graph.incoming_edges(node.id);
            std::string prev_steps;
            for (const auto* e : in_edges) {
                const Node* prev = graph.find_node(e->from);
                if (prev && prev->type != NodeType::START) {
                    if (!prev_steps.empty()) prev_steps += ", ";
                    prev_steps += prev->label();
                    if (previous_node_id.empty()) {
                        previous_node_id = prev->id;
                    }
                }
            }

            pipeline_context = "Pipeline: " + pipeline_name + "\n";
            pipeline_context += "Current step: " + node.label() + "\n";
            if (!next_steps.empty()) {
                pipeline_context += "Next step: " + next_steps + "\n";
            }
            if (!prev_steps.empty()) {
                pipeline_context += "Previous step: " + prev_steps + "\n";
            }
        }

        std::string result;
        bool go_back = false;

        if (session_) {
            // Server mode: populate session, emit event, block on CV
            {
                std::lock_guard<std::mutex> lock(session_->mutex);
                session_->node_id = node.id;
                session_->prompt = prompt;
                session_->context_summary = context_summary;
                session_->pipeline_context = pipeline_context;
                session_->previous_node_id = previous_node_id;
                session_->active = true;
                session_->continued = false;
                session_->go_back = false;
                session_->final_result.clear();
            }

            // Emit HUMAN_QUESTION with interactive flag and context
            {
                PipelineEvent event;
                event.type = EventType::HUMAN_QUESTION;
                event.timestamp = utc_timestamp_now();
                event.node_id = node.id;
                event.message = prompt;
                nlohmann::json data;
                data["interactive"] = true;
                data["prompt"] = prompt;
                data["context_summary"] = context_summary;
                data["pipeline_context"] = pipeline_context;
                data["node_label"] = node.label();
                event.data = std::move(data);
                exec_ctx.event_bus.emit(event);
            }

            // Block until the /continue endpoint signals us — OR until the
            // engine's cancellation flag flips (e.g. server shutdown). Without
            // the cancelled check, a paused interactive node would hold the
            // run_thread forever on shutdown, forcing SIGKILL.
            {
                std::unique_lock<std::mutex> lock(session_->mutex);
                session_->cv.wait(lock, [this, &exec_ctx]{
                    return session_->continued || exec_ctx.cancelled.load();
                });
                if (exec_ctx.cancelled.load() && !session_->continued) {
                    // Shutdown path: leave the session in a clean state and
                    // return SUCCESS with empty output. The engine's outer
                    // loop will observe cancelled and unwind.
                    session_->active = false;
                    NEEDLE_LOG_INFO("interactive",
                        "node %s: cancellation observed during wait — returning",
                        node.id.c_str());
                    Outcome cancel_outcome;
                    cancel_outcome.status = StageStatus::SUCCESS;
                    return Result<Outcome>::success(std::move(cancel_outcome));
                }
                result = session_->final_result;
                go_back = session_->go_back;
                session_->active = false;
                session_->continued = false;
                session_->go_back = false;
            }

            // Emit answer event
            {
                PipelineEvent event;
                event.type = EventType::HUMAN_ANSWER;
                event.timestamp = utc_timestamp_now();
                event.node_id = node.id;
                event.message = "Interactive stage completed: " + node.id;
                exec_ctx.event_bus.emit(event);
            }
        } else {
            // No session (CLI / test mode): emit events with empty result
            {
                PipelineEvent event;
                event.type = EventType::HUMAN_QUESTION;
                event.timestamp = utc_timestamp_now();
                event.node_id = node.id;
                event.message = prompt;
                nlohmann::json data;
                data["interactive"] = false;
                data["prompt"] = prompt;
                data["context_summary"] = context_summary;
                data["pipeline_context"] = pipeline_context;
                data["node_label"] = node.label();
                event.data = std::move(data);
                exec_ctx.event_bus.emit(event);
            }

            {
                PipelineEvent event;
                event.type = EventType::HUMAN_ANSWER;
                event.timestamp = utc_timestamp_now();
                event.node_id = node.id;
                event.message = "Interactive stage completed: " + node.id;
                exec_ctx.event_bus.emit(event);
            }
        }

        // Write result to stage dir
        if (!exec_ctx.logs_root.empty()) {
            std::string stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
            platform::mkdir_p(stage_dir);
            std::ofstream out(stage_dir + "/response.md");
            if (out.is_open()) out << result;
        }

        Outcome outcome;
        outcome.status = StageStatus::SUCCESS;
        outcome.output = result;
        outcome.context_updates["interactive." + node.id + ".output"] = result;

        if (go_back && !previous_node_id.empty()) {
            outcome.preferred_label = "revise";
            outcome.suggested_next = {previous_node_id};
        }

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<Backend> backend_;
    std::shared_ptr<InteractiveSession> session_;
};

std::shared_ptr<Handler> make_interactive_handler(std::shared_ptr<Backend> backend,
                                                   std::shared_ptr<InteractiveSession> session) {
    return std::make_shared<InteractiveHandler>(std::move(backend), std::move(session));
}

} // namespace needle
