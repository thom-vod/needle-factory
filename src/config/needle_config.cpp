#include "needle/config/needle_config.h"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

#include "needle/util/fs_helpers.h"
#include "needle/util/logger.h"

namespace needle {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Split a dot-notation path into segments.
std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string segment;
    for (char c : path) {
        if (c == '.') {
            if (!segment.empty()) {
                parts.push_back(segment);
                segment.clear();
            }
        } else {
            segment += c;
        }
    }
    if (!segment.empty()) {
        parts.push_back(segment);
    }
    return parts;
}

/// Deep-merge `src` into `dst`. Values in `src` overwrite `dst` for non-object
/// types; objects are merged recursively.
void deep_merge(nlohmann::json& dst, const nlohmann::json& src) {
    if (!src.is_object()) {
        dst = src;
        return;
    }
    if (!dst.is_object()) {
        dst = src;
        return;
    }
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it.value().is_object() && dst.count(it.key()) && dst[it.key()].is_object()) {
            deep_merge(dst[it.key()], it.value());
        } else {
            dst[it.key()] = it.value();
        }
    }
}

/// Map a provider name to the conventional env var for its API key.
std::string env_var_for_provider(const std::string& provider) {
    // Lowercase compare
    std::string p = provider;
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);

    if (p == "openai")     return "OPENAI_API_KEY";
    if (p == "anthropic")  return "ANTHROPIC_API_KEY";
    if (p == "gemini")     return "GEMINI_API_KEY";
    if (p == "tavily")     return "TAVILY_API_KEY";
    return "";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

NeedleConfig::NeedleConfig()
    : data_(nlohmann::json::object()) {}

// ---------------------------------------------------------------------------
// Static accessors
// ---------------------------------------------------------------------------

NeedleConfig& NeedleConfig::global() {
    static NeedleConfig instance;
    return instance;
}

std::string NeedleConfig::config_path() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) return "";
    return std::string(home) + "/.needle/config.json";
}

void NeedleConfig::set_config_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_path_override_ = path;
}

std::string NeedleConfig::effective_config_path() const {
    if (!config_path_override_.empty()) {
        return config_path_override_;
    }
    return config_path();
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

nlohmann::json NeedleConfig::defaults() const {
    return nlohmann::json{
        {"schema_version", 1},
        {"providers", {
            {"openai",     {{"api_key", ""}, {"default_model", "gpt-4o"}}},
            {"anthropic",  {{"api_key", ""}, {"default_model", "claude-sonnet-4-6"}}},
            {"gemini",     {{"api_key", ""}, {"default_model", "gemini-2.5-pro"}}},
            {"tavily",     {{"api_key", ""}}}
        }},
        {"defaults", {
            {"chat_agent",        "claude"},
            {"chat_model",        "claude-sonnet-4-6"},
            {"coding_agent",      "codex"},
            {"coding_model",      "gpt-5.4"},
            {"planning_agent",    "claude"},
            {"planning_model",    "claude-opus-4-7"},
            {"critique_agent",    "gemini"},
            {"critique_model",    "gemini-2.5-pro"},
            {"codergen_timeout",  "45m"},
            {"max_context_value_kb", 100},
            // Prompt-size guards (N5). A 333 KB prompt observed in production
            // led to a 45-min stall; warn early so misshapen DOTs surface
            // before burning a full attempt.
            {"prompt_warn_kb", 100},
            {"prompt_fail_kb", 500}
        }},
        {"server", {
            {"port",         8080},
            {"bind",         "127.0.0.1"},
            {"auto_approve", false}
        }},
        {"worktree", {
            {"strategy",        "off"},
            {"branch_template", "auto/${run_id}"},
            {"path_template",   "../${repo_basename}-wt-${run_id}"},
            {"cleanup",         "keep"}
        }},
        {"ui", {
            {"theme", "dark"}
        }},
        {"logging", {
            {"level", "info"}
        }}
    };
}

void NeedleConfig::ensure_defaults() {
    nlohmann::json def = defaults();
    // Merge defaults under data_ — existing keys in data_ take precedence.
    // We deep-merge default into a copy, then deep-merge data_ on top.
    deep_merge(def, data_);
    data_ = std::move(def);
}

// ---------------------------------------------------------------------------
// JSON path helpers
// ---------------------------------------------------------------------------

nlohmann::json NeedleConfig::walk_path(const nlohmann::json& j, const std::string& path) {
    std::vector<std::string> parts = split_path(path);
    const nlohmann::json* cur = &j;
    for (const auto& p : parts) {
        if (!cur->is_object() || cur->find(p) == cur->end()) {
            return nlohmann::json(nullptr);
        }
        cur = &(*cur)[p];
    }
    return *cur;
}

void NeedleConfig::set_path(nlohmann::json& j, const std::string& path, const nlohmann::json& value) {
    std::vector<std::string> parts = split_path(path);
    if (parts.empty()) return;

    nlohmann::json* cur = &j;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!cur->is_object()) {
            *cur = nlohmann::json::object();
        }
        if (cur->find(parts[i]) == cur->end() || !(*cur)[parts[i]].is_object()) {
            (*cur)[parts[i]] = nlohmann::json::object();
        }
        cur = &(*cur)[parts[i]];
    }
    if (!cur->is_object()) {
        *cur = nlohmann::json::object();
    }
    (*cur)[parts.back()] = value;
}

void NeedleConfig::remove_path(nlohmann::json& j, const std::string& path) {
    std::vector<std::string> parts = split_path(path);
    if (parts.empty()) return;

    nlohmann::json* cur = &j;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!cur->is_object() || cur->find(parts[i]) == cur->end()) {
            return; // Path doesn't exist
        }
        cur = &(*cur)[parts[i]];
    }
    if (cur->is_object()) {
        cur->erase(parts.back());
    }
}

// ---------------------------------------------------------------------------
// Redaction
// ---------------------------------------------------------------------------

nlohmann::json NeedleConfig::redact(const nlohmann::json& j) {
    if (j.is_object()) {
        nlohmann::json result = nlohmann::json::object();
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key().find("api_key") != std::string::npos && it.value().is_string()) {
                std::string val = it.value().get<std::string>();
                if (val.empty()) {
                    result[it.key()] = "not set";
                } else if (val.size() <= 5) {
                    result[it.key()] = val + "***";
                } else {
                    result[it.key()] = val.substr(0, 5) + "***";
                }
            } else {
                result[it.key()] = redact(it.value());
            }
        }
        return result;
    }
    if (j.is_array()) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& elem : j) {
            result.push_back(redact(elem));
        }
        return result;
    }
    return j;
}

// ---------------------------------------------------------------------------
// Load / Save
// ---------------------------------------------------------------------------

Result<void> NeedleConfig::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string path = effective_config_path();
    if (path.empty()) {
        data_ = defaults();
        NEEDLE_LOG_WARN("config", "HOME not set, using built-in defaults");
        return Result<void>::success();
    }

    if (!is_file(path)) {
        // No config file — just use defaults. Don't create the file yet.
        data_ = defaults();
        NEEDLE_LOG_DEBUG("config", "No config file at %s, using defaults", path.c_str());
        return Result<void>::success();
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        data_ = defaults();
        NEEDLE_LOG_WARN("config", "Cannot open %s, using defaults", path.c_str());
        return Result<void>::success();
    }

    try {
        f >> data_;
    } catch (const nlohmann::json::parse_error& e) {
        data_ = defaults();
        NEEDLE_LOG_ERROR("config", "Parse error in %s: %s", path.c_str(), e.what());
        return Result<void>::failure(std::string("JSON parse error: ") + e.what());
    }

    ensure_defaults();
    NEEDLE_LOG_DEBUG("config", "Loaded config from %s", path.c_str());
    return Result<void>::success();
}

Result<void> NeedleConfig::save() const {
    // NOTE: mutex_ must already be held by the caller or this is called from
    // a method that holds it. For the public save(), we acquire it here.
    // However, since mutable mutex_ can be locked in a const method and we
    // call save() from non-const methods that already hold the lock, we need
    // a separate internal save. To keep it simple: this public method acquires
    // the lock, and internal callers use save_locked_().
    //
    // Actually, let's just make save() acquire the lock and provide an internal
    // helper that assumes the lock is held.
    std::lock_guard<std::mutex> lock(mutex_);
    return save_impl();
}

Result<void> NeedleConfig::save_impl() const {
    std::string path = effective_config_path();
    if (path.empty()) {
        return Result<void>::failure("Cannot determine config path (HOME not set)");
    }

    // Ensure parent directory exists
    std::string::size_type last_slash = path.rfind('/');
    if (last_slash != std::string::npos) {
        std::string dir = path.substr(0, last_slash);
        if (!mkdir_p(dir)) {
            return Result<void>::failure("Cannot create directory: " + dir);
        }
    }

    // Write to temp file, then rename (atomic)
    std::string tmp_path = path + ".tmp";
    {
        std::ofstream f(tmp_path);
        if (!f.is_open()) {
            return Result<void>::failure("Cannot write to " + tmp_path);
        }
        f << data_.dump(2) << std::endl;
        if (f.bad()) {
            return Result<void>::failure("Write error to " + tmp_path);
        }
    }

    // Rename for atomic replacement
#ifdef _WIN32
    // On Windows, rename fails if destination exists — remove it first
    std::remove(path.c_str());
#endif
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return Result<void>::failure("Cannot rename temp file to " + path);
    }

#ifndef _WIN32
    // Set permissions to 0600 (not meaningful on Windows)
    ::chmod(path.c_str(), 0600);
#endif

    NEEDLE_LOG_DEBUG("config", "Saved config to %s", path.c_str());
    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

std::string NeedleConfig::get_string(const std::string& path,
                                     const std::string& env_var,
                                     const std::string& fallback) const {
    // Env var takes top precedence
    if (!env_var.empty()) {
        const char* val = std::getenv(env_var.c_str());
        if (val && val[0] != '\0') {
            return val;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json v = walk_path(data_, path);
    if (!v.is_null() && v.is_string()) {
        return v.get<std::string>();
    }
    return fallback;
}

bool NeedleConfig::get_bool(const std::string& path, bool fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json v = walk_path(data_, path);
    if (!v.is_null() && v.is_boolean()) {
        return v.get<bool>();
    }
    return fallback;
}

int NeedleConfig::get_int(const std::string& path, int fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json v = walk_path(data_, path);
    if (!v.is_null() && v.is_number_integer()) {
        return v.get<int>();
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------

Result<void> NeedleConfig::set(const std::string& path, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    set_path(data_, path, nlohmann::json(value));
    return save_impl();
}

Result<void> NeedleConfig::set_bool(const std::string& path, bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    set_path(data_, path, nlohmann::json(value));
    return save_impl();
}

Result<void> NeedleConfig::unset(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    remove_path(data_, path);
    return save_impl();
}

// ---------------------------------------------------------------------------
// JSON access
// ---------------------------------------------------------------------------

nlohmann::json NeedleConfig::to_json_redacted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return redact(data_);
}

nlohmann::json NeedleConfig::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_;
}

// ---------------------------------------------------------------------------
// Merge
// ---------------------------------------------------------------------------

Result<void> NeedleConfig::merge(const nlohmann::json& partial) {
    std::lock_guard<std::mutex> lock(mutex_);
    deep_merge(data_, partial);
    return save_impl();
}

// ---------------------------------------------------------------------------
// API key resolution
// ---------------------------------------------------------------------------

std::string NeedleConfig::resolve_api_key(const std::string& provider) const {
    // Check env var first
    std::string env = env_var_for_provider(provider);
    if (!env.empty()) {
        const char* val = std::getenv(env.c_str());
        if (val && val[0] != '\0') {
            return val;
        }
    }

    // Fall back to config
    std::lock_guard<std::mutex> lock(mutex_);
    std::string config_path_key = "providers." + provider + ".api_key";
    nlohmann::json v = walk_path(data_, config_path_key);
    if (!v.is_null() && v.is_string()) {
        return v.get<std::string>();
    }
    return "";
}

} // namespace needle
