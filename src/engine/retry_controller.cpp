#include "needle/engine/retry_controller.h"
#include <thread>
#include <chrono>

namespace needle {

bool RetryController::should_retry(const std::string& node_id, const RetryPolicy& policy) const {
    int used = attempts(node_id);
    return used < policy.max_retries;
}

void RetryController::record_attempt(const std::string& node_id) {
    counters_[node_id]++;
}

int RetryController::attempts(const std::string& node_id) const {
    auto it = counters_.find(node_id);
    if (it != counters_.end()) {
        return it->second;
    }
    return 0;
}

void RetryController::sleep_before_retry(const std::string& node_id, const RetryPolicy& policy) const {
    int attempt = attempts(node_id);
    int delay_ms = policy.delay_for_attempt(attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

nlohmann::json RetryController::to_json() const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : counters_) {
        j[kv.first] = kv.second;
    }
    return j;
}

RetryController RetryController::from_json(const nlohmann::json& j) {
    RetryController rc;
    for (auto it = j.begin(); it != j.end(); ++it) {
        rc.counters_[it.key()] = it.value().get<int>();
    }
    return rc;
}

} // namespace needle
