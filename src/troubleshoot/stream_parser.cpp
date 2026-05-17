#include "needle/troubleshoot/stream_parser.h"

#include <algorithm>
#include <sstream>

namespace needle {

namespace {

const size_t kPreviewLimit = 240;

std::string truncate(std::string s, size_t limit = kPreviewLimit) {
    if (s.size() <= limit) return s;
    if (limit <= 3) return s.substr(0, limit);
    return s.substr(0, limit - 3) + "...";
}

std::string json_summary(const nlohmann::json& j) {
    if (j.is_null()) return "";
    if (j.is_string()) return truncate(j.get<std::string>());
    return truncate(j.dump());
}

const nlohmann::json* message_content(const nlohmann::json& j) {
    if (j.contains("message") && j["message"].is_object() &&
        j["message"].contains("content")) {
        return &j["message"]["content"];
    }
    if (j.contains("content")) return &j["content"];
    return nullptr;
}

void add_raw(std::vector<TroubleshootStreamEvent>& out,
             const std::string& raw_line,
             const nlohmann::json& event) {
    TroubleshootStreamEvent e;
    e.type = "raw";
    e.raw_line = raw_line;
    e.payload["raw"] = event;
    out.push_back(std::move(e));
}

bool bool_value(const nlohmann::json& j, const char* key, bool fallback = false) {
    return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : fallback;
}

std::string string_value(const nlohmann::json& j, const char* key,
                         const std::string& fallback = "") {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}

} // namespace

std::vector<TroubleshootStreamEvent>
TroubleshootStreamParser::parse_line(const std::string& line) {
    std::vector<TroubleshootStreamEvent> out;
    if (line.empty()) return out;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const std::exception&) {
        return out;
    }
    if (!j.is_object()) return out;

    const std::string raw_type = string_value(j, "type");
    const std::string subtype = string_value(j, "subtype");

    if (raw_type == "system" && subtype == "init") {
        if (!emitted_session_started_) {
            emitted_session_started_ = true;
            TroubleshootStreamEvent e;
            e.type = "session_started";
            e.raw_line = line;
            e.payload["claude_session_id"] = string_value(j, "session_id");
            e.payload["model"] = string_value(j, "model");
            out.push_back(std::move(e));
        }
        return out;
    }

    if (raw_type == "assistant") {
        const nlohmann::json* content = message_content(j);
        if (content && content->is_array()) {
            for (const auto& block : *content) {
                if (!block.is_object() || string_value(block, "type") != "tool_use") {
                    continue;
                }
                TroubleshootStreamEvent e;
                e.type = "tool_call";
                e.raw_line = line;
                e.payload["tool"] = string_value(block, "name", "tool");
                e.payload["tool_use_id"] = string_value(block, "id");
                e.payload["input_summary"] = block.contains("input")
                    ? json_summary(block["input"]) : "";
                if (j.contains("elapsed_ms") && j["elapsed_ms"].is_number()) {
                    e.payload["elapsed_ms"] = j["elapsed_ms"];
                }
                out.push_back(std::move(e));
            }
        }
        // SPRINT-016 M5 fix: plain-text assistant messages (no tool_use
        // blocks) must NOT fall through to add_raw — that would leak the
        // model's raw text into the SSE stream. Drop the event entirely;
        // it still lands in events.ndjson via the caller.
        return out;
    }

    if (raw_type == "user") {
        const nlohmann::json* content = message_content(j);
        if (content && content->is_array()) {
            for (const auto& block : *content) {
                if (!block.is_object() || string_value(block, "type") != "tool_result") {
                    continue;
                }
                TroubleshootStreamEvent e;
                e.type = "tool_result";
                e.raw_line = line;
                e.payload["tool"] = string_value(block, "name", "tool_result");
                e.payload["tool_use_id"] = string_value(block, "tool_use_id");
                const bool is_error = bool_value(block, "is_error", false);
                e.payload["ok"] = !is_error;
                e.payload["status"] = is_error ? "err" : "ok";
                e.payload["output_preview"] = block.contains("content")
                    ? json_summary(block["content"]) : "";
                out.push_back(std::move(e));
            }
        }
        // SPRINT-016 M5 fix: same no-raw-fallthrough rule for user blocks
        // carrying plain-text content (chat replies during escalation).
        return out;
    }

    if (raw_type == "result") {
        TroubleshootStreamEvent completed;
        completed.type = "session_completed";
        completed.raw_line = line;
        if (subtype == "success" && !bool_value(j, "is_error", false)) {
            completed.payload["outcome"] = "resumed";
        } else {
            completed.payload["outcome"] = "failed_agent";
        }
        completed.payload["subtype"] = subtype;
        // SPRINT-016 M5 fix: truncate the agent's final result string to
        // kPreviewLimit so megabytes of operator chat fragments don't
        // make it into SSE payloads.
        completed.payload["summary"] = truncate(string_value(j, "result"));
        if (j.contains("num_turns")) completed.payload["num_turns"] = j["num_turns"];
        if (j.contains("usage")) completed.payload["usage"] = j["usage"];
        out.push_back(std::move(completed));

        if (j.contains("total_cost_usd") && j["total_cost_usd"].is_number()) {
            TroubleshootStreamEvent cost;
            cost.type = "cost_update";
            cost.raw_line = line;
            cost.payload["cost_usd"] = j["total_cost_usd"];
            if (j.contains("num_turns")) cost.payload["num_turns"] = j["num_turns"];
            if (j.contains("usage")) cost.payload["usage"] = j["usage"];
            out.push_back(std::move(cost));
        }
        return out;
    }

    if (raw_type == "rate_limit_event") {
        add_raw(out, line, j);
        return out;
    }

    add_raw(out, line, j);
    return out;
}

} // namespace needle
