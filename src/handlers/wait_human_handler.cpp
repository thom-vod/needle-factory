#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/interviewer/interviewer.h"
#include "needle/event/event.h"
#include <memory>

namespace needle {

class WaitHumanHandler : public HandlerBase {
public:
    explicit WaitHumanHandler(std::shared_ptr<Interviewer> interviewer)
        : interviewer_(std::move(interviewer)) {}

    std::string type_name() const override { return "wait_human"; }

    Result<Outcome> do_execute(const Node& node, Context& ctx,
                               const ExecutionContext& exec_ctx) override {
        (void)ctx;
        // Derive choices from outgoing edge labels
        auto edges = exec_ctx.graph.outgoing_edges(node.id);
        std::vector<std::string> choices;
        for (const auto* edge : edges) {
            std::string lbl = edge->label();
            if (lbl.empty()) {
                lbl = edge->to;
            }
            choices.push_back(lbl);
        }

        if (choices.empty()) {
            return Result<Outcome>::failure("no choices available for wait_human node: " + node.id);
        }

        // Build question
        InterviewQuestion question;
        question.prompt = node.prompt().empty() ? node.label() : node.prompt();
        question.choices = choices;

        // Emit HUMAN_QUESTION event
        {
            PipelineEvent event;
            event.type = EventType::HUMAN_QUESTION;
            event.timestamp = utc_timestamp_now();
            event.node_id = node.id;
            event.message = question.prompt;
            nlohmann::json data;
            data["choices"] = choices;
            event.data = std::move(data);
            exec_ctx.event_bus.emit(event);
        }

        // Ask interviewer
        auto answer_result = interviewer_->ask(question);
        if (!answer_result.ok()) {
            return Result<Outcome>::failure("interviewer failed: " + answer_result.error());
        }

        InterviewAnswer answer = answer_result.value();

        // Emit HUMAN_ANSWER event
        {
            PipelineEvent event;
            event.type = EventType::HUMAN_ANSWER;
            event.timestamp = utc_timestamp_now();
            event.node_id = node.id;
            event.message = "User selected: " + choices[answer.selected_index];
            nlohmann::json data;
            data["selected_index"] = answer.selected_index;
            data["selected_choice"] = choices[answer.selected_index];
            data["raw_input"] = answer.raw_input;
            event.data = std::move(data);
            exec_ctx.event_bus.emit(event);
        }

        Outcome outcome;
        outcome.status = StageStatus::SUCCESS;
        outcome.preferred_label = choices[answer.selected_index];
        outcome.output = "user chose: " + choices[answer.selected_index];

        // Store feedback in context so retry nodes can reference it
        outcome.context_updates["human.gate.selected"] = choices[answer.selected_index];
        if (!answer.raw_input.empty()) {
            outcome.context_updates["human.gate.feedback"] = answer.raw_input;
        }

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<Interviewer> interviewer_;
};

std::shared_ptr<Handler> make_wait_human_handler(std::shared_ptr<Interviewer> interviewer) {
    return std::make_shared<WaitHumanHandler>(std::move(interviewer));
}

} // namespace needle
