#include "needle/engine/stage_advancer.h"

#include "needle/engine/checkpoint_manager.h"
#include "needle/util/fs_helpers.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace needle {

namespace {

std::string checkpoint_path(const std::string& run_dir) {
    return run_dir + "/checkpoint.json";
}

std::string stage_status_path(const std::string& run_dir, const std::string& node_id) {
    return run_dir + "/stages/" + node_id + "/status.json";
}

void write_stage_status(const std::string& path, const nlohmann::json& status) {
    // Best-effort directory creation. mkdir_p is in fs_helpers.
    std::string::size_type slash = path.rfind('/');
    if (slash != std::string::npos) {
        mkdir_p(path.substr(0, slash));
    }
    std::ofstream out(path);
    if (out.is_open()) {
        out << status.dump(2);
    }
}

} // anonymous

Result<void> StageAdvancer::mark(const std::string& run_dir,
                                 const std::string& node_id,
                                 bool success,
                                 const std::string& output_text) {
    JsonCheckpointWriter writer;
    auto load_result = writer.load(checkpoint_path(run_dir));
    if (!load_result.ok()) {
        return Result<void>::failure("cannot load checkpoint: " + load_result.error());
    }
    Checkpoint cp = std::move(load_result.value());

    auto in_completed = std::find(cp.completed_nodes.begin(),
                                  cp.completed_nodes.end(), node_id);

    if (success) {
        if (in_completed == cp.completed_nodes.end()) {
            cp.completed_nodes.push_back(node_id);
        }
        cp.context.set("needle.last_outcome.status", "SUCCESS");
        // Codergen and tool nodes use different context-update prefixes.
        // We set both so downstream lookups under either prefix find the
        // marked output text — overkill but harmless and avoids requiring
        // the caller to know which handler owns the node.
        cp.context.set("codergen." + node_id + ".output", output_text);
        cp.context.set("tool." + node_id + ".output", output_text);
    } else {
        if (in_completed != cp.completed_nodes.end()) {
            cp.completed_nodes.erase(in_completed);
        }
        cp.context.set("needle.last_outcome.status", "FAILURE");
    }
    cp.current_node = node_id;

    auto save_result = writer.save(cp, checkpoint_path(run_dir));
    if (!save_result.ok()) {
        return save_result;
    }

    // Update or create the stage's status.json.
    nlohmann::json status;
    status["node_id"] = node_id;
    status["status"] = success ? "SUCCESS" : "FAILURE";
    status["output"] = output_text;
    status["preferred_label"] = "";
    status["context_updates"] = nlohmann::json::object();
    write_stage_status(stage_status_path(run_dir, node_id), status);

    return Result<void>::success();
}

Result<void> StageAdvancer::advance(const std::string& run_dir,
                                    const std::string& target_node_id) {
    if (target_node_id.empty()) {
        return Result<void>::failure("advance requires a target node id");
    }

    JsonCheckpointWriter writer;
    auto load_result = writer.load(checkpoint_path(run_dir));
    if (!load_result.ok()) {
        return Result<void>::failure("cannot load checkpoint: " + load_result.error());
    }
    Checkpoint cp = std::move(load_result.value());
    cp.current_node = target_node_id;

    return writer.save(cp, checkpoint_path(run_dir));
}

} // namespace needle
