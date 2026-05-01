#pragma once

#include <string>
#include <vector>
#include "needle/model/graph.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/validation/diagnostic.h"

namespace needle {

class ResumeValidator {
public:
    // Validate checkpoint against graph. Errors block resume, warnings proceed.
    //
    // `strict_hash_check`: when true, a hash mismatch on a *completed* node
    // (i.e., the operator edited a stage that has already run) is escalated
    // to ERROR. Default is soft: edits to unstarted nodes are silent, edits
    // to started nodes warn but proceed. The CLI flag `--strict-graph-hash`
    // and the graph attribute `strict_hash_check=true` both flip this.
    static Diagnostics validate(const Checkpoint& cp,
                                const Graph& graph,
                                bool strict_hash_check = false);

    // Compute structural+semantic hash: sorted node IDs + handler types + edge pairs + edge conditions.
    static std::string compute_graph_hash(const Graph& graph);

    // Compute a per-node hash that includes id + handler + every attribute.
    // Used for the soft-hash check: when the overall graph hash differs but
    // each completed node's per-node hash still matches, we know only
    // unstarted nodes were edited — safe to continue.
    static std::string compute_node_hash(const Node& node);
};

} // namespace needle
