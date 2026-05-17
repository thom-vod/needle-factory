#include <catch2/catch.hpp>

#include "needle/troubleshoot/stream_parser.h"

using namespace needle;

TEST_CASE("TroubleshootStreamParser maps system init once", "[stream_parser]") {
    TroubleshootStreamParser parser;
    auto events = parser.parse_line(R"({"type":"system","subtype":"init","session_id":"abc","model":"claude-opus-4-7"})");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == "session_started");
    REQUIRE(events[0].payload["claude_session_id"] == "abc");

    auto again = parser.parse_line(R"({"type":"system","subtype":"init","session_id":"def"})");
    REQUIRE(again.empty());
}

TEST_CASE("TroubleshootStreamParser maps assistant tool_use", "[stream_parser]") {
    TroubleshootStreamParser parser;
    auto events = parser.parse_line(R"({"type":"assistant","message":{"content":[{"type":"text","text":"checking"},{"type":"tool_use","id":"toolu_1","name":"Read","input":{"file_path":"/tmp/status.json"}}]},"elapsed_ms":42})");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == "tool_call");
    REQUIRE(events[0].payload["tool"] == "Read");
    REQUIRE(events[0].payload["tool_use_id"] == "toolu_1");
    REQUIRE(events[0].payload["elapsed_ms"] == 42);
    REQUIRE(events[0].payload["input_summary"].get<std::string>().find("status.json") != std::string::npos);
}

TEST_CASE("TroubleshootStreamParser maps user tool_result", "[stream_parser]") {
    TroubleshootStreamParser parser;
    auto events = parser.parse_line(R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"status: failed","is_error":false}]}})");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == "tool_result");
    REQUIRE(events[0].payload["tool_use_id"] == "toolu_1");
    REQUIRE(events[0].payload["ok"] == true);
    REQUIRE(events[0].payload["output_preview"] == "status: failed");
}

TEST_CASE("TroubleshootStreamParser maps result success to completion and cost", "[stream_parser]") {
    TroubleshootStreamParser parser;
    auto events = parser.parse_line(R"({"type":"result","subtype":"success","is_error":false,"result":"resumed","total_cost_usd":0.43,"num_turns":3,"usage":{"input_tokens":10,"output_tokens":5}})");
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == "session_completed");
    REQUIRE(events[0].payload["outcome"] == "resumed");
    REQUIRE(events[0].payload["num_turns"] == 3);
    REQUIRE(events[1].type == "cost_update");
    REQUIRE(events[1].payload["cost_usd"] == 0.43);
    REQUIRE(events[1].payload["usage"]["input_tokens"] == 10);
}

TEST_CASE("TroubleshootStreamParser maps error result to failed completion", "[stream_parser]") {
    TroubleshootStreamParser parser;
    auto events = parser.parse_line(R"({"type":"result","subtype":"error_max_turns","is_error":true,"result":"failed"})");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == "session_completed");
    REQUIRE(events[0].payload["outcome"] == "failed_agent");
}

TEST_CASE("TroubleshootStreamParser drops plain-text assistant blocks (no raw leak)", "[stream_parser][m5]") {
    TroubleshootStreamParser parser;
    // SPRINT-016 M5 regression: a plain-text assistant message with no
    // tool_use blocks must NOT fall through to a raw event — that
    // would leak the model's response text to SSE clients.
    auto events = parser.parse_line(
        R"({"type":"assistant","message":{"content":[{"type":"text","text":"hidden sensitive content"}]}})");
    REQUIRE(events.empty());
}

TEST_CASE("TroubleshootStreamParser drops plain-text user blocks (no raw leak)", "[stream_parser][m5]") {
    TroubleshootStreamParser parser;
    // SPRINT-016 M5 regression for user-side text (e.g. operator
    // replies during escalation).
    auto events = parser.parse_line(
        R"({"type":"user","message":{"content":[{"type":"text","text":"hidden operator message"}]}})");
    REQUIRE(events.empty());
}

TEST_CASE("TroubleshootStreamParser truncates result.result to preview length", "[stream_parser][m5]") {
    TroubleshootStreamParser parser;
    // SPRINT-016 M5 regression: unbounded result.result was a leak vector.
    std::string huge(2000, 'x');
    std::string line = R"({"type":"result","subtype":"success","is_error":false,"result":")" + huge + R"(","total_cost_usd":0.01})";
    auto events = parser.parse_line(line);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == "session_completed");
    const std::string summary = events[0].payload["summary"].get<std::string>();
    REQUIRE(summary.size() < huge.size());
    REQUIRE(summary.size() <= 240);
}

TEST_CASE("TroubleshootStreamParser maps rate limit and unknown events to raw", "[stream_parser]") {
    TroubleshootStreamParser parser;
    auto rate = parser.parse_line(R"({"type":"rate_limit_event","retry_after_ms":1200})");
    REQUIRE(rate.size() == 1);
    REQUIRE(rate[0].type == "raw");
    REQUIRE(rate[0].payload["raw"]["retry_after_ms"] == 1200);

    auto unknown = parser.parse_line(R"({"type":"content_block_delta","delta":{"text":"x"}})");
    REQUIRE(unknown.size() == 1);
    REQUIRE(unknown[0].type == "raw");

    auto bad = parser.parse_line("{not json");
    REQUIRE(bad.empty());
}
