#ifdef NEEDLE_ENABLE_SERVER

#include "needle/server/model_cache.h"

namespace needle {

std::pair<bool, nlohmann::json> ModelCache::get_if_fresh(const std::string& provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ts_it = timestamps_.find(provider);
    if (ts_it == timestamps_.end()) {
        return {false, nlohmann::json()};
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - ts_it->second).count();
    if (elapsed >= TTL_SECONDS) {
        return {false, nlohmann::json()};
    }
    auto model_it = models_.find(provider);
    if (model_it == models_.end()) {
        return {false, nlohmann::json()};
    }
    return {true, model_it->second};
}

void ModelCache::store(const std::string& provider, const nlohmann::json& models) {
    std::lock_guard<std::mutex> lock(mutex_);
    models_[provider] = models;
    timestamps_[provider] = std::chrono::steady_clock::now();
}

void ModelCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    models_.clear();
    timestamps_.clear();
}

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
