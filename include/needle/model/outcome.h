#pragma once

#include <string>
#include <vector>
#include <map>

namespace needle {

enum class StageStatus {
    SUCCESS,
    PARTIAL_SUCCESS,
    FAILURE,
    RETRY,
    SKIP
};

std::string to_string(StageStatus status);
StageStatus stage_status_from_string(const std::string& s);

struct Outcome {
    StageStatus status;
    std::string preferred_label;
    std::vector<std::string> suggested_next;
    std::map<std::string, std::string> context_updates;
    std::string output;
    int retry_after_ms = 0;  // Server-specified retry delay (0 = use policy default)
};

} // namespace needle
