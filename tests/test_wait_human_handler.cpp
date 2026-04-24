#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/interviewer/interviewer.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include <atomic>

using namespace needle;

namespace {

Graph make_human_graph() {
    std::vector<Node> nodes;

    Node human;
    human.id = "review";
    human.type = NodeType::WAIT_HUMAN;
    human.attrs.set("prompt", "Approve the changes?");
    nodes.push_back(std::move(human));

    Node approve;
    approve.id = "approve";
    approve.type = NodeType::CODERGEN;
    nodes.push_back(std::move(approve));

    Node reject;
    reject.id = "reject";
    reject.type = NodeType::CODERGEN;
    nodes.push_back(std::move(reject));

    std::vector<Edge> edges;

    Edge e1;
    e1.from = "review";
    e1.to = "approve";
    e1.attrs.set("label", "yes");
    edges.push_back(std::move(e1));

    Edge e2;
    e2.from = "review";
    e2.to = "reject";
    e2.attrs.set("label", "no");
    edges.push_back(std::move(e2));

    return Graph::make("human_test", std::move(nodes), std::move(edges));
}

} // anonymous namespace

TEST_CASE("WaitHumanHandler: choices derived from edges, answer routes correctly", "[wait_human]") {
    // Queue the answer: select index 0 ("yes")
    InterviewAnswer ans;
    ans.selected_index = 0;
    ans.raw_input = "1";
    auto queue = std::make_shared<QueueInterviewer>(std::vector<InterviewAnswer>{ans});
    auto handler = make_wait_human_handler(queue);

    Graph graph = make_human_graph();
    const Node* review = graph.find_node("review");
    REQUIRE(review != nullptr);

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    // Track events
    std::vector<EventType> events;
    bus.subscribe([&events](const PipelineEvent& e) {
        events.push_back(e.type);
    });

    auto result = handler->execute(*review, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().preferred_label == "yes");

    // Should have emitted HUMAN_QUESTION and HUMAN_ANSWER events
    bool has_question = false;
    bool has_answer = false;
    for (auto et : events) {
        if (et == EventType::HUMAN_QUESTION) has_question = true;
        if (et == EventType::HUMAN_ANSWER) has_answer = true;
    }
    REQUIRE(has_question);
    REQUIRE(has_answer);
}

TEST_CASE("WaitHumanHandler: second choice routes to 'no'", "[wait_human]") {
    InterviewAnswer ans;
    ans.selected_index = 1;
    ans.raw_input = "2";
    auto queue = std::make_shared<QueueInterviewer>(std::vector<InterviewAnswer>{ans});
    auto handler = make_wait_human_handler(queue);

    Graph graph = make_human_graph();
    const Node* review = graph.find_node("review");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(*review, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().preferred_label == "no");
}

TEST_CASE("WaitHumanHandler: with RecordingInterviewer records Q&A", "[wait_human]") {
    InterviewAnswer ans;
    ans.selected_index = 0;
    ans.raw_input = "1";
    auto inner = std::make_shared<QueueInterviewer>(std::vector<InterviewAnswer>{ans});
    auto recorder = std::make_shared<RecordingInterviewer>(inner);
    auto handler = make_wait_human_handler(recorder);

    Graph graph = make_human_graph();
    const Node* review = graph.find_node("review");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    handler->execute(*review, ctx, exec_ctx);

    REQUIRE(recorder->recording().size() == 1);
    REQUIRE(recorder->recording()[0].first.choices.size() == 2);
    REQUIRE(recorder->recording()[0].second.selected_index == 0);
}
