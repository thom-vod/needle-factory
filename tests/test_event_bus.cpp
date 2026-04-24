#include <catch2/catch.hpp>
#include "needle/event/event.h"
#include "needle/event/event_bus.h"
#include "needle/event/collector_event_bus.h"
#include <thread>
#include <atomic>
#include <vector>

using namespace needle;

static PipelineEvent make_event(EventType type, const std::string& msg) {
    PipelineEvent e;
    e.type = type;
    e.timestamp = utc_timestamp_now();
    e.node_id = "";
    e.message = msg;
    e.data = nlohmann::json::object();
    return e;
}

// ====================== PipelineEvent ======================

TEST_CASE("PipelineEvent: to_json", "[event]") {
    PipelineEvent e;
    e.type = EventType::PIPELINE_STARTED;
    e.timestamp = "2026-03-12T00:00:00Z";
    e.node_id = "start";
    e.message = "Pipeline started";
    e.data = nlohmann::json::object();
    e.data["key"] = "value";

    auto j = e.to_json();
    REQUIRE(j["type"] == "PIPELINE_STARTED");
    REQUIRE(j["timestamp"] == "2026-03-12T00:00:00Z");
    REQUIRE(j["node_id"] == "start");
    REQUIRE(j["message"] == "Pipeline started");
    REQUIRE(j["data"]["key"] == "value");
}

TEST_CASE("event_type_to_string covers all types", "[event]") {
    REQUIRE(event_type_to_string(EventType::PIPELINE_STARTED) == "PIPELINE_STARTED");
    REQUIRE(event_type_to_string(EventType::PIPELINE_COMPLETED) == "PIPELINE_COMPLETED");
    REQUIRE(event_type_to_string(EventType::PIPELINE_FAILED) == "PIPELINE_FAILED");
    REQUIRE(event_type_to_string(EventType::STAGE_STARTED) == "STAGE_STARTED");
    REQUIRE(event_type_to_string(EventType::STAGE_COMPLETED) == "STAGE_COMPLETED");
    REQUIRE(event_type_to_string(EventType::STAGE_FAILED) == "STAGE_FAILED");
    REQUIRE(event_type_to_string(EventType::STAGE_RETRYING) == "STAGE_RETRYING");
    REQUIRE(event_type_to_string(EventType::PARALLEL_BRANCH_STARTED) == "PARALLEL_BRANCH_STARTED");
    REQUIRE(event_type_to_string(EventType::PARALLEL_BRANCH_COMPLETED) == "PARALLEL_BRANCH_COMPLETED");
    REQUIRE(event_type_to_string(EventType::HUMAN_QUESTION) == "HUMAN_QUESTION");
    REQUIRE(event_type_to_string(EventType::HUMAN_ANSWER) == "HUMAN_ANSWER");
    REQUIRE(event_type_to_string(EventType::CHECKPOINT_SAVED) == "CHECKPOINT_SAVED");
}

TEST_CASE("utc_timestamp_now returns ISO 8601 format", "[event]") {
    std::string ts = utc_timestamp_now();
    // Should be like "2026-03-12T12:34:56Z"
    REQUIRE(ts.size() == 20);
    REQUIRE(ts[4] == '-');
    REQUIRE(ts[7] == '-');
    REQUIRE(ts[10] == 'T');
    REQUIRE(ts[13] == ':');
    REQUIRE(ts[16] == ':');
    REQUIRE(ts[19] == 'Z');
}

// ====================== EventBus ======================

TEST_CASE("EventBus: subscribe and emit", "[event][eventbus]") {
    EventBus bus;
    int count = 0;
    bus.subscribe([&count](const PipelineEvent&) { ++count; });

    auto e = make_event(EventType::PIPELINE_STARTED, "start");
    bus.emit(e);
    REQUIRE(count == 1);

    bus.emit(e);
    REQUIRE(count == 2);
}

TEST_CASE("EventBus: multiple subscribers", "[event][eventbus]") {
    EventBus bus;
    int count_a = 0, count_b = 0;
    bus.subscribe([&count_a](const PipelineEvent&) { ++count_a; });
    bus.subscribe([&count_b](const PipelineEvent&) { ++count_b; });

    auto e = make_event(EventType::STAGE_STARTED, "stage");
    bus.emit(e);
    REQUIRE(count_a == 1);
    REQUIRE(count_b == 1);
}

TEST_CASE("EventBus: thread safety - emit from multiple threads", "[event][eventbus]") {
    EventBus bus;
    std::atomic<int> count(0);
    bus.subscribe([&count](const PipelineEvent&) { ++count; });

    const int num_threads = 4;
    const int events_per_thread = 100;
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&bus]() {
            for (int j = 0; j < 100; ++j) {
                auto e = make_event(EventType::STAGE_COMPLETED, "done");
                bus.emit(e);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    REQUIRE(count == num_threads * events_per_thread);
}

TEST_CASE("EventBus: subscribe during emit from another thread", "[event][eventbus]") {
    EventBus bus;
    std::atomic<int> count(0);
    bus.subscribe([&count](const PipelineEvent&) { ++count; });

    // Subscribe from one thread, emit from another
    std::thread emitter([&bus]() {
        for (int i = 0; i < 50; ++i) {
            auto e = make_event(EventType::STAGE_STARTED, "s");
            bus.emit(e);
        }
    });

    std::thread subscriber([&bus]() {
        for (int i = 0; i < 10; ++i) {
            bus.subscribe([](const PipelineEvent&) {});
        }
    });

    emitter.join();
    subscriber.join();

    // Just verify no crash/deadlock
    REQUIRE(count >= 50);
}

// ====================== CollectorEventBus ======================

TEST_CASE("CollectorEventBus: record and retrieve events", "[event][collector]") {
    CollectorEventBus collector;
    collector.record(make_event(EventType::PIPELINE_STARTED, "start"));
    collector.record(make_event(EventType::STAGE_STARTED, "stage1"));
    collector.record(make_event(EventType::PIPELINE_COMPLETED, "done"));

    auto events = collector.events();
    REQUIRE(events.size() == 3);
    REQUIRE(events[0].type == EventType::PIPELINE_STARTED);
    REQUIRE(events[1].type == EventType::STAGE_STARTED);
    REQUIRE(events[2].type == EventType::PIPELINE_COMPLETED);
}

TEST_CASE("CollectorEventBus: replay_to", "[event][collector]") {
    CollectorEventBus collector;
    collector.record(make_event(EventType::PIPELINE_STARTED, "start"));
    collector.record(make_event(EventType::STAGE_STARTED, "stage1"));

    std::vector<PipelineEvent> replayed;
    collector.replay_to([&replayed](const PipelineEvent& e) {
        replayed.push_back(e);
    });

    REQUIRE(replayed.size() == 2);
    REQUIRE(replayed[0].message == "start");
    REQUIRE(replayed[1].message == "stage1");
}

TEST_CASE("CollectorEventBus: empty collector", "[event][collector]") {
    CollectorEventBus collector;
    REQUIRE(collector.events().empty());

    int count = 0;
    collector.replay_to([&count](const PipelineEvent&) { ++count; });
    REQUIRE(count == 0);
}

TEST_CASE("CollectorEventBus: thread safety", "[event][collector]") {
    CollectorEventBus collector;
    const int num_threads = 4;
    const int events_per_thread = 50;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&collector]() {
            for (int j = 0; j < 50; ++j) {
                collector.record(make_event(EventType::STAGE_COMPLETED, "done"));
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(collector.events().size() ==
            static_cast<size_t>(num_threads * events_per_thread));
}
