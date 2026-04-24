#pragma once

#ifdef NEEDLE_ENABLE_SERVER

#include <string>
#include <map>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>

namespace needle {

// Thread-safe model list cache with TTL (M6).
// Single mutex scope covers check + store to eliminate TOCTOU race.
// The fetch occurs outside the lock scope — if two threads miss simultaneously,
// both fetch but only the second store is a harmless overwrite.
class ModelCache {
public:
    // Returns {true, models_json} if cache is fresh, {false, {}} otherwise.
    // Caller must hold no locks when calling this.
    std::pair<bool, nlohmann::json> get_if_fresh(const std::string& provider);

    // Store a freshly fetched model list. Safe to call from multiple threads.
    void store(const std::string& provider, const nlohmann::json& models);

    // Clear all cached entries (for testing)
    void clear();

    static const int TTL_SECONDS = 300;  // 5 minutes

private:
    std::mutex mutex_;
    std::map<std::string, nlohmann::json> models_;
    std::map<std::string, std::chrono::steady_clock::time_point> timestamps_;
};

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
