#pragma once

#include <string>
#include <map>
#include <nlohmann/json.hpp>
#include "needle/model/retry_policy.h"

namespace needle {

class RetryController {
public:
    bool should_retry(const std::string& node_id, const RetryPolicy& policy) const;
    void record_attempt(const std::string& node_id);
    void reset() { counters_.clear(); }
    int attempts(const std::string& node_id) const;
    void sleep_before_retry(const std::string& node_id, const RetryPolicy& policy) const;

    nlohmann::json to_json() const;
    static RetryController from_json(const nlohmann::json& j);

private:
    std::map<std::string, int> counters_;
};

} // namespace needle
