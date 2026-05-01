#pragma once

#include <string>
#include "needle/model/result.h"

namespace needle {

// Mechanically advance a checkpoint. Replaces the manual five-edit dance
// (completed_nodes / context["codergen.<n>.output"] / last_outcome.status /
// current_node / stages/<n>/status.json) with two CLI-friendly operations.
//
// Used by the `needle stage` CLI verbs and by the troubleshoot-agent
// (Sprint 6+). All operations are atomic via temp-file-and-rename through
// JsonCheckpointWriter; idempotent against re-invocation with same args.
class StageAdvancer {
public:
    // Mark a stage's status — typically used after a manual recovery to flip
    // a FAILURE checkpoint entry to SUCCESS so resume continues.
    //
    // - run_dir: directory containing checkpoint.json + stages/<node>/status.json
    // - node_id: which stage to mark
    // - success: true = SUCCESS (added to completed_nodes); false = FAILURE
    //   (removed from completed_nodes, status.json marked FAILURE)
    // - output_text: optional summary text written into context as
    //   `codergen.<node_id>.output` and into the stage's status.json.
    static Result<void> mark(const std::string& run_dir,
                             const std::string& node_id,
                             bool success,
                             const std::string& output_text);

    // Set the checkpoint's current_node so a subsequent `needle resume`
    // continues from there. Caller specifies the target explicitly — the
    // graph-aware "what's next from here" lookup belongs to a higher layer
    // (troubleshoot-agent, Sprint 6+) since it requires loading the graph.
    static Result<void> advance(const std::string& run_dir,
                                const std::string& target_node_id);
};

} // namespace needle
