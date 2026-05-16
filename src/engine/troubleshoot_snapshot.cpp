#include "needle/engine/troubleshoot_snapshot.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "needle/config/needle_config.h"
#include "needle/platform/platform.h"

namespace needle {

namespace {

std::string basename_of(const std::string& path) {
    size_t end = path.find_last_not_of("/\\");
    if (end == std::string::npos) return path;
    size_t start = path.find_last_of("/\\", end);
    return path.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
}

std::string stem_of(const std::string& path) {
    std::string base = basename_of(path);
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

Result<void> copy_file(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in.is_open()) return Result<void>::failure("cannot read " + from);
    size_t slash = to.find_last_of("/\\");
    if (slash != std::string::npos && !platform::mkdir_p(to.substr(0, slash))) {
        return Result<void>::failure("cannot create directory for " + to);
    }
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return Result<void>::failure("cannot write " + to);
    out << in.rdbuf();
    return Result<void>::success();
}

std::string infer_run_dir_from_session(const std::string& session_dir) {
    size_t troubleshoot = session_dir.find_last_of("/\\");
    if (troubleshoot == std::string::npos) return "";
    std::string parent = session_dir.substr(0, troubleshoot);
    size_t run = parent.find_last_of("/\\");
    if (run == std::string::npos) return "";
    if (basename_of(parent) != "troubleshoot") return "";
    return parent.substr(0, run);
}

} // namespace

Result<void> TroubleshootSnapshot::capture(const std::string& project_dir,
                                           const std::string& graph_path,
                                           const std::string& session_dir,
                                           TroubleshootMode mode) {
    if (mode == TroubleshootMode::Off || mode == TroubleshootMode::Diagnose) {
        return Result<void>::success();
    }

    const std::string snapshot_dir = session_dir + "/snapshot";
    if (!platform::mkdir_p(snapshot_dir)) {
        return Result<void>::failure("cannot create snapshot directory " + snapshot_dir);
    }

    nlohmann::json manifest;
    manifest["graph_path"] = graph_path;
    manifest["prompts"] = nlohmann::json::array();
    manifest["config_path"] = nullptr;

    if (!graph_path.empty() && platform::is_regular_file(graph_path)) {
        std::string graph_snapshot = snapshot_dir + "/" + basename_of(graph_path);
        auto copied = copy_file(graph_path, graph_snapshot);
        if (!copied.ok()) return copied;
        manifest["graph_snapshot"] = graph_snapshot;
    }

    std::string stem = !graph_path.empty() ? stem_of(graph_path) : "";
    if (stem.empty()) {
        std::string run_dir = infer_run_dir_from_session(session_dir);
        stem = basename_of(run_dir);
    }
    const std::string stages_dir = project_dir + "/.needle/" + stem + "/stages";
    if (platform::is_directory(stages_dir)) {
        for (const auto& node : platform::list_directory(stages_dir)) {
            const std::string prompt_path = stages_dir + "/" + node + "/prompt.md";
            if (!platform::is_regular_file(prompt_path)) continue;
            const std::string prompt_snapshot = snapshot_dir + "/prompt.md." + node;
            auto copied = copy_file(prompt_path, prompt_snapshot);
            if (!copied.ok()) return copied;
            manifest["prompts"].push_back({
                {"node", node},
                {"path", prompt_path},
                {"snapshot", prompt_snapshot}
            });
        }
    }

    const std::string config_path = NeedleConfig::global().config_path();
    if (!config_path.empty() && platform::is_regular_file(config_path)) {
        const std::string config_snapshot = snapshot_dir + "/config.json";
        auto copied = copy_file(config_path, config_snapshot);
        if (!copied.ok()) return copied;
        manifest["config_path"] = config_path;
        manifest["config_snapshot"] = config_snapshot;
    }

    std::ofstream out(snapshot_dir + "/manifest.json");
    if (!out.is_open()) return Result<void>::failure("cannot write snapshot manifest");
    out << manifest.dump(2);
    return Result<void>::success();
}

Result<void> TroubleshootSnapshot::restore(const std::string& project_dir,
                                           const std::string& session_dir) {
    (void)project_dir;
    const std::string manifest_path = session_dir + "/snapshot/manifest.json";
    std::ifstream in(manifest_path);
    if (!in.is_open()) {
        return Result<void>::failure("snapshot manifest not found: " + manifest_path);
    }
    auto manifest = nlohmann::json::parse(in, nullptr, false);
    if (!manifest.is_object()) return Result<void>::failure("invalid snapshot manifest");

    if (manifest.contains("graph_path") && manifest["graph_path"].is_string()) {
        const std::string graph_path = manifest["graph_path"].get<std::string>();
        const std::string graph_snapshot = manifest.value("graph_snapshot", "");
        if (!graph_path.empty() && !graph_snapshot.empty() && platform::is_regular_file(graph_snapshot)) {
            auto copied = copy_file(graph_snapshot, graph_path);
            if (!copied.ok()) return copied;
        }
    }

    if (manifest.contains("prompts") && manifest["prompts"].is_array()) {
        for (const auto& prompt : manifest["prompts"]) {
            if (!prompt.is_object()) continue;
            std::string path = prompt.value("path", "");
            std::string snapshot = prompt.value("snapshot", "");
            if (!path.empty() && !snapshot.empty() && platform::is_regular_file(snapshot)) {
                auto copied = copy_file(snapshot, path);
                if (!copied.ok()) return copied;
            }
        }
    }

    std::string config_path;
    std::string config_snapshot;
    if (manifest.contains("config_path") && manifest["config_path"].is_string()) {
        config_path = manifest["config_path"].get<std::string>();
    }
    if (manifest.contains("config_snapshot") && manifest["config_snapshot"].is_string()) {
        config_snapshot = manifest["config_snapshot"].get<std::string>();
    }
    if (!config_path.empty() && !config_snapshot.empty() && platform::is_regular_file(config_snapshot)) {
        auto copied = copy_file(config_snapshot, config_path);
        if (!copied.ok()) return copied;
    }

    return Result<void>::success();
}

} // namespace needle
