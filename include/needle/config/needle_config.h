#pragma once

#include <string>
#include <mutex>
#include <nlohmann/json.hpp>
#include "needle/model/result.h"

namespace needle {

class NeedleConfig {
public:
    NeedleConfig();

    // Load from ~/.needle/config.json. Uses defaults if file missing.
    Result<void> load();

    // Save current state to file (0600 permissions).
    Result<void> save() const;

    // Get a value by dot-notation path (e.g. "providers.openai.api_key").
    // Falls back: env var > config file > built-in default.
    std::string get_string(const std::string& path,
                           const std::string& env_var = "",
                           const std::string& fallback = "") const;

    bool get_bool(const std::string& path, bool fallback = false) const;
    int get_int(const std::string& path, int fallback = 0) const;

    // Set a value by dot-notation path. Writes to file immediately.
    Result<void> set(const std::string& path, const std::string& value);
    Result<void> set_bool(const std::string& path, bool value);

    // Remove a key.
    Result<void> unset(const std::string& path);

    // Get the full config as JSON (with keys redacted for display).
    nlohmann::json to_json_redacted() const;

    // Get raw JSON (for API merge operations).
    nlohmann::json to_json() const;

    // Merge partial JSON into config. Writes to file.
    Result<void> merge(const nlohmann::json& partial);

    // Resolve an API key: env var takes precedence, then config.
    std::string resolve_api_key(const std::string& provider) const;

    // Static global accessor.
    static NeedleConfig& global();

    // Config file path.
    static std::string config_path();

    // Override the config file path (for testing).
    void set_config_path(const std::string& path);

private:
    nlohmann::json data_;
    mutable std::mutex mutex_;
    std::string config_path_override_;

    std::string effective_config_path() const;

    nlohmann::json defaults() const;
    void ensure_defaults();
    static nlohmann::json walk_path(const nlohmann::json& j, const std::string& path);
    static void set_path(nlohmann::json& j, const std::string& path, const nlohmann::json& value);
    static void remove_path(nlohmann::json& j, const std::string& path);
    static nlohmann::json redact(const nlohmann::json& j);

    // Internal save that assumes mutex_ is already held.
    Result<void> save_impl() const;
};

} // namespace needle
