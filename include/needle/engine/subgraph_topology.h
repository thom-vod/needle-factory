#pragma once

#include <string>
#include <set>
#include <vector>
#include "needle/model/graph.h"

namespace needle {

// SubgraphTopology: BFS-based graph traversal helpers for
// parallel fan-in discovery (M15), manager-loop body collection (M12),
// and reachability analysis.
class SubgraphTopology {
public:
    // Collect all node IDs reachable from `start` via BFS, stopping at any node in `stop_nodes`.
    // Stop nodes themselves are NOT included in the result.
    static std::set<std::string> collect_reachable(
        const Graph& g,
        const std::string& start,
        const std::set<std::string>& stop_nodes);

    // Find the common convergence node (FAN_IN) reachable from all branch starts.
    // If multiple common fan-ins exist, pick the nearest (shortest BFS distance) with lexical tiebreak.
    static std::string find_common_fan_in(
        const Graph& g,
        const std::vector<std::string>& branch_starts);

    // Collect manager-loop body nodes: all nodes reachable from `first_body_node`
    // before hitting `manager_id` again (the loop-back edge target).
    // Includes `first_body_node` itself but not `manager_id`.
    static std::set<std::string> collect_loop_body(
        const Graph& g,
        const std::string& manager_id,
        const std::string& first_body_node);
};

} // namespace needle
