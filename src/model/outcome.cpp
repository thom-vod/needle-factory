#include "needle/model/outcome.h"

namespace needle {

std::string to_string(StageStatus status) {
    switch (status) {
        case StageStatus::SUCCESS:         return "SUCCESS";
        case StageStatus::PARTIAL_SUCCESS: return "PARTIAL_SUCCESS";
        case StageStatus::FAILURE:         return "FAILURE";
        case StageStatus::RETRY:           return "RETRY";
        case StageStatus::SKIP:            return "SKIP";
    }
    return "FAILURE";
}

StageStatus stage_status_from_string(const std::string& s) {
    if (s == "SUCCESS")         return StageStatus::SUCCESS;
    if (s == "PARTIAL_SUCCESS") return StageStatus::PARTIAL_SUCCESS;
    if (s == "RETRY")           return StageStatus::RETRY;
    if (s == "SKIP")            return StageStatus::SKIP;
    return StageStatus::FAILURE;
}

} // namespace needle
