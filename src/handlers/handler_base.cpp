#include "needle/handlers/handler_base.h"
#include "needle/event/event.h"
#include "needle/util/fs_helpers.h"

#include <fstream>
#include <stdexcept>

namespace needle {

Result<Outcome> HandlerBase::execute(const Node& node, Context& ctx,
                                     const ExecutionContext& exec_ctx) {
    NEEDLE_LOG_DEBUG("handler", "handler %s execute node=%s",
                     type_name().c_str(), node.id.c_str());

    Result<Outcome> result = Result<Outcome>::failure("uninitialized");
    try {
        result = do_execute(node, ctx, exec_ctx);
    } catch (const std::exception& ex) {
        NEEDLE_LOG_ERROR("handler", "handler %s threw exception on node=%s: %s",
                         type_name().c_str(), node.id.c_str(), ex.what());
        return Result<Outcome>::failure(
            std::string("exception in handler ") + type_name() + ": " + ex.what());
    }

    if (!result.ok()) {
        NEEDLE_LOG_ERROR("handler", "handler %s failed node=%s: %s",
                         type_name().c_str(), node.id.c_str(), result.error().c_str());
        return result;
    }

    Outcome outcome = result.value();

    // Contract check: FAILURE outcomes must have a non-empty output message
    if (outcome.status == StageStatus::FAILURE && outcome.output.empty()) {
        NEEDLE_LOG_WARN("handler", "handler %s returned FAILURE with empty output for node=%s, setting default",
                        type_name().c_str(), node.id.c_str());
        outcome.output = "stage failed (no details from handler " + type_name() + ")";
    }

    // Write artifact to project dir if the node specifies one
    std::string artifact = node.attrs.get("artifact");
    if (!artifact.empty() && outcome.status == StageStatus::SUCCESS &&
        !outcome.output.empty() && !exec_ctx.project_dir.empty()) {
        std::string artifact_path = exec_ctx.project_dir + "/" + artifact;
        // Ensure parent directory exists
        std::string::size_type last_slash = artifact_path.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            mkdir_p(artifact_path.substr(0, last_slash));
        }
        std::ofstream af(artifact_path);
        if (af.is_open()) {
            af << outcome.output;
            NEEDLE_LOG_INFO("handler", "wrote artifact %s for node=%s",
                            artifact.c_str(), node.id.c_str());
        }
    }

    NEEDLE_LOG_DEBUG("handler", "handler %s completed node=%s status=%s",
                     type_name().c_str(), node.id.c_str(),
                     to_string(outcome.status).c_str());

    return Result<Outcome>::success(std::move(outcome));
}

void HandlerBase::emit_warning(const ExecutionContext& exec_ctx,
                               const std::string& node_id,
                               const std::string& message) {
    PipelineEvent e;
    e.type = EventType::STAGE_WARNING;
    e.timestamp = utc_timestamp_now();
    e.node_id = node_id;
    e.message = message;
    e.data = nlohmann::json::object();
    e.data["warning"] = message;
    exec_ctx.event_bus.emit(e);
}

void HandlerBase::write_stage_file(const ExecutionContext& exec_ctx,
                                   const std::string& node_id,
                                   const std::string& filename,
                                   const std::string& content) {
    if (exec_ctx.logs_root.empty()) return;

    std::string dir = exec_ctx.logs_root + "/stages/" + node_id;
    mkdir_p(dir);

    std::string path = dir + "/" + filename;
    std::ofstream out(path);
    if (out.is_open()) {
        out << content;
    }
}

Outcome HandlerBase::make_failure(const std::string& error_message) {
    Outcome o;
    o.status = StageStatus::FAILURE;
    o.output = error_message;
    return o;
}

Outcome HandlerBase::make_success(const std::string& output) {
    Outcome o;
    o.status = StageStatus::SUCCESS;
    o.output = output;
    return o;
}

Outcome HandlerBase::make_skip(const std::string& reason) {
    Outcome o;
    o.status = StageStatus::SUCCESS;
    o.output = reason;
    return o;
}

} // namespace needle
