#include "needle/util/run_registry.h"
#include "needle/platform/platform.h"

#include <fstream>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <cctype>
#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace needle {

std::string RunRegistry::default_registry_path() {
    const char* override_path = std::getenv("NEEDLE_RUNS_PATH");
    if (override_path) return std::string(override_path);
    return platform::home_dir() + "/.needle/runs.json";
}

std::string RunRegistry::registry_path() const {
    return path_override_.empty() ? default_registry_path() : path_override_;
}

std::string RunRegistry::generate_run_id(const std::string& project_dir) {
    std::string prefix;
    if (!project_dir.empty() && project_dir != ".") {
        std::string dir = project_dir;
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
        auto slash = dir.find_last_of("/\\");
        prefix = (slash != std::string::npos) ? dir.substr(slash + 1) : dir;
        std::string clean;
        for (char c : prefix) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
                clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        prefix = clean;
    }
    if (prefix.empty()) prefix = "run";

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char ts[16];
    std::strftime(ts, sizeof(ts), "%y%m%d-%H%M", &tm_buf);

    return prefix + "-" + ts;
}

Result<void> RunRegistry::load() {
    if (!enabled_) return Result<void>::success();
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = registry_path();
    std::ifstream f(path);
    if (!f.is_open()) {
        data_ = {{"version", 1}, {"runs", nlohmann::json::object()}};
        return Result<void>::success();
    }
    try {
        f >> data_;
        if (!data_.contains("runs")) data_["runs"] = nlohmann::json::object();
    } catch (...) {
        data_ = {{"version", 1}, {"runs", nlohmann::json::object()}};
    }
    return Result<void>::success();
}

Result<void> RunRegistry::save() const {
    if (!enabled_) return Result<void>::success();
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = registry_path();
    size_t parent_end = path.find_last_of("/\\");
    if (parent_end != std::string::npos) {
        std::string parent = path.substr(0, parent_end);
        if (!parent.empty() && !platform::mkdir_p(parent)) {
            return Result<void>::failure("cannot create directory " + parent);
        }
    }
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp);
    if (!out.is_open()) return Result<void>::failure("cannot write " + tmp);
    out << data_.dump(2);
    out.close();
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
    return Result<void>::success();
}

void RunRegistry::add_entry(const std::string& id,
                            const std::string& dot_stem,
                            const std::string& dot_source,
                            const std::string& project_dir,
                            const std::string& logs_root,
                            const std::string& status,
                            const std::string& created_at,
                            bool dry_run) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json entry;
    entry["id"] = id;
    entry["dot_stem"] = dot_stem;
    entry["dot_source"] = dot_source;
    entry["project_dir"] = project_dir;
    entry["logs_root"] = logs_root;
    entry["status"] = status;
    entry["dry_run"] = dry_run;
    entry["error"] = "";
    entry["created_at"] = created_at;
    data_["runs"][id] = entry;
}

void RunRegistry::update_status(const std::string& id, const std::string& status,
                                const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (data_["runs"].contains(id)) {
        data_["runs"][id]["status"] = status;
        data_["runs"][id]["error"] = error;
    }
}

void RunRegistry::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_["runs"].erase(id);
}

std::vector<nlohmann::json> RunRegistry::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> result;
    if (!data_.contains("runs")) return result;
    for (auto it = data_["runs"].begin(); it != data_["runs"].end(); ++it) {
        nlohmann::json entry = it.value();
        if (!entry.contains("dry_run")) entry["dry_run"] = false;
        result.push_back(std::move(entry));
    }
    return result;
}

} // namespace needle
