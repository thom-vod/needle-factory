#include "needle/engine/checkpoint_manager.h"
#include "needle/event/event.h"
#include "needle/util/logger.h"

#include <fstream>
#include <cstdio>

namespace needle {

nlohmann::json Checkpoint::to_json() const {
    nlohmann::json j;
    j["timestamp"] = timestamp;
    j["current_node"] = current_node;
    j["completed_nodes"] = completed_nodes;

    nlohmann::json rc = nlohmann::json::object();
    for (const auto& kv : retry_counters) {
        rc[kv.first] = kv.second;
    }
    j["retry_counters"] = rc;

    j["context"] = context.to_json();
    j["graph_file"] = graph_file;
    j["graph_hash"] = graph_hash;

    nlohmann::json cnh = nlohmann::json::object();
    for (const auto& kv : completed_node_hashes) {
        cnh[kv.first] = kv.second;
    }
    j["completed_node_hashes"] = cnh;

    nlohmann::json bwt = nlohmann::json::object();
    for (const auto& kv : branch_worktrees) {
        bwt[kv.first] = kv.second;
    }
    j["branch_worktrees"] = bwt;

    if (!stylesheet_file.empty()) j["stylesheet_file"] = stylesheet_file;
    if (!logs_root.empty()) j["logs_root"] = logs_root;
    return j;
}

Result<Checkpoint> Checkpoint::from_json(const nlohmann::json& j) {
    Checkpoint cp;
    try {
        cp.timestamp = j.at("timestamp").get<std::string>();
        cp.current_node = j.at("current_node").get<std::string>();

        for (const auto& n : j.at("completed_nodes")) {
            cp.completed_nodes.push_back(n.get<std::string>());
        }

        if (j.count("retry_counters")) {
            for (auto it = j["retry_counters"].begin(); it != j["retry_counters"].end(); ++it) {
                cp.retry_counters[it.key()] = it.value().get<int>();
            }
        }

        cp.context = Context::from_json(j.at("context"));
        cp.graph_file = j.value("graph_file", std::string());
        cp.graph_hash = j.value("graph_hash", std::string());

        if (j.count("completed_node_hashes")) {
            for (auto it = j["completed_node_hashes"].begin();
                 it != j["completed_node_hashes"].end(); ++it) {
                cp.completed_node_hashes[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.count("branch_worktrees")) {
            for (auto it = j["branch_worktrees"].begin();
                 it != j["branch_worktrees"].end(); ++it) {
                cp.branch_worktrees[it.key()] = it.value().get<std::string>();
            }
        }

        if (j.count("stylesheet_file")) cp.stylesheet_file = j["stylesheet_file"].get<std::string>();
        if (j.count("logs_root")) cp.logs_root = j["logs_root"].get<std::string>();

        return Result<Checkpoint>::success(std::move(cp));
    } catch (const std::exception& e) {
        return Result<Checkpoint>::failure(std::string("failed to parse checkpoint: ") + e.what());
    }
}

// JsonCheckpointWriter

Result<void> JsonCheckpointWriter::save(const Checkpoint& cp, const std::string& path) {
    NEEDLE_LOG_DEBUG("checkpoint", "saving checkpoint to %s (node: %s)", path.c_str(), cp.current_node.c_str());
    std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path);
        if (!out.is_open()) {
            return Result<void>::failure("failed to open temp file: " + tmp_path);
        }
        out << cp.to_json().dump(2);
        if (!out.good()) {
            return Result<void>::failure("failed to write checkpoint to: " + tmp_path);
        }
    }

    // On Windows, std::rename fails if the destination already exists — remove first
    std::remove(path.c_str());
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        NEEDLE_LOG_ERROR("checkpoint", "failed to rename %s -> %s", tmp_path.c_str(), path.c_str());
        return Result<void>::failure("failed to rename checkpoint file");
    }

    return Result<void>::success();
}

Result<Checkpoint> JsonCheckpointWriter::load(const std::string& path) {
    NEEDLE_LOG_DEBUG("checkpoint", "loading checkpoint from %s", path.c_str());
    std::ifstream in(path);
    if (!in.is_open()) {
        return Result<Checkpoint>::failure("failed to open checkpoint file: " + path);
    }

    try {
        nlohmann::json j;
        in >> j;
        return Checkpoint::from_json(j);
    } catch (const std::exception& e) {
        return Result<Checkpoint>::failure(std::string("failed to parse checkpoint JSON: ") + e.what());
    }
}

// InMemoryCheckpointWriter

Result<void> InMemoryCheckpointWriter::save(const Checkpoint& cp, const std::string& path) {
    store_[path] = cp.to_json();
    return Result<void>::success();
}

Result<Checkpoint> InMemoryCheckpointWriter::load(const std::string& path) {
    auto it = store_.find(path);
    if (it == store_.end()) {
        return Result<Checkpoint>::failure("checkpoint not found: " + path);
    }
    return Checkpoint::from_json(it->second);
}

} // namespace needle
