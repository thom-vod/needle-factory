#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace needle {

enum class EventType {
    PIPELINE_STARTED,
    PIPELINE_COMPLETED,
    PIPELINE_FAILED,
    STAGE_STARTED,
    STAGE_COMPLETED,
    STAGE_FAILED,
    STAGE_RETRYING,
    PARALLEL_BRANCH_STARTED,
    PARALLEL_BRANCH_COMPLETED,
    HUMAN_QUESTION,
    HUMAN_ANSWER,
    CHECKPOINT_SAVED,
    VARIABLE_UNRESOLVED,
    RESUME_WARNING,
    STAGE_WARNING,
    PIPELINE_PAUSED,
    PIPELINE_RESUMED,
    STAGE_PAUSED
};

inline std::string event_type_to_string(EventType t) {
    switch (t) {
        case EventType::PIPELINE_STARTED:           return "PIPELINE_STARTED";
        case EventType::PIPELINE_COMPLETED:         return "PIPELINE_COMPLETED";
        case EventType::PIPELINE_FAILED:            return "PIPELINE_FAILED";
        case EventType::STAGE_STARTED:              return "STAGE_STARTED";
        case EventType::STAGE_COMPLETED:            return "STAGE_COMPLETED";
        case EventType::STAGE_FAILED:               return "STAGE_FAILED";
        case EventType::STAGE_RETRYING:             return "STAGE_RETRYING";
        case EventType::PARALLEL_BRANCH_STARTED:    return "PARALLEL_BRANCH_STARTED";
        case EventType::PARALLEL_BRANCH_COMPLETED:  return "PARALLEL_BRANCH_COMPLETED";
        case EventType::HUMAN_QUESTION:             return "HUMAN_QUESTION";
        case EventType::HUMAN_ANSWER:               return "HUMAN_ANSWER";
        case EventType::CHECKPOINT_SAVED:           return "CHECKPOINT_SAVED";
        case EventType::VARIABLE_UNRESOLVED:        return "VARIABLE_UNRESOLVED";
        case EventType::RESUME_WARNING:             return "RESUME_WARNING";
        case EventType::STAGE_WARNING:              return "STAGE_WARNING";
        case EventType::PIPELINE_PAUSED:            return "PIPELINE_PAUSED";
        case EventType::PIPELINE_RESUMED:           return "PIPELINE_RESUMED";
        case EventType::STAGE_PAUSED:               return "STAGE_PAUSED";
    }
    return "UNKNOWN";
}

std::string utc_timestamp_now();

struct PipelineEvent {
    EventType type;
    std::string timestamp;        // ISO 8601 UTC
    std::string node_id;          // "" for pipeline-level events
    std::string message;          // human-readable description
    nlohmann::json data;          // event-specific structured data

    nlohmann::json to_json() const;
};

} // namespace needle
