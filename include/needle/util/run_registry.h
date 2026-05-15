#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>
#include "needle/model/result.h"

namespace needle {

/// Standalone run registry that persists to ~/.needle/runs.json by default.
/// Set NEEDLE_RUNS_PATH to override the default process-wide path, or use the
/// explicit-path constructor in tests to avoid env-var coupling.
/// Thread-safe with file locking for concurrent CLI/server access.
class RunRegistry {
public:
    RunRegistry() = default;
    /// Persist this registry to a specific path, bypassing NEEDLE_RUNS_PATH.
    explicit RunRegistry(const std::string& path) : path_override_(path) {}

    Result<void> load();
    Result<void> save() const;

    /// Add a run entry from raw fields (CLI-friendly — no PipelineRun dependency).
    void add_entry(const std::string& id,
                   const std::string& dot_stem,
                   const std::string& dot_source,
                   const std::string& project_dir,
                   const std::string& logs_root,
                   const std::string& status,
                   const std::string& created_at);

    void update_status(const std::string& id, const std::string& status,
                       const std::string& error = "");
    void remove(const std::string& id);
    std::vector<nlohmann::json> all() const;
    std::string registry_path() const;

    static std::string default_registry_path();
    static std::string generate_run_id(const std::string& project_dir);

    void set_enabled(bool enabled) { enabled_ = enabled; }

private:
    nlohmann::json data_;
    mutable std::mutex mutex_;
    std::string path_override_;
    bool enabled_ = true;
};

} // namespace needle
