#include "needle/engine/subgraph_topology.h"
#include <queue>

namespace needle {

std::set<std::string> SubgraphTopology::collect_reachable(
    const Graph& g,
    const std::string& start,
    const std::set<std::string>& stop_nodes)
{
    std::set<std::string> result;
    std::set<std::string> visited;
    std::queue<std::string> q;

    q.push(start);
    visited.insert(start);

    while (!q.empty()) {
        std::string node_id = q.front();
        q.pop();

        // Stop nodes are boundaries — don't include them or traverse past them
        if (stop_nodes.count(node_id)) {
            continue;
        }

        result.insert(node_id);

        auto edges = g.outgoing_edges(node_id);
        for (const auto* e : edges) {
            if (visited.find(e->to) == visited.end()) {
                visited.insert(e->to);
                q.push(e->to);
            }
        }
    }

    return result;
}

std::string SubgraphTopology::find_common_fan_in(
    const Graph& g,
    const std::vector<std::string>& branch_starts)
{
    if (branch_starts.empty()) return "";

    // For each branch, collect all reachable FAN_IN nodes via BFS
    std::vector<std::set<std::string>> per_branch_fan_ins;

    for (const auto& start : branch_starts) {
        std::set<std::string> fan_ins;
        std::set<std::string> visited;
        std::queue<std::string> bfs_queue;

        bfs_queue.push(start);
        visited.insert(start);

        while (!bfs_queue.empty()) {
            std::string node_id = bfs_queue.front();
            bfs_queue.pop();

            const Node* node = g.find_node(node_id);
            if (!node) continue;

            if (node->type == NodeType::FAN_IN) {
                fan_ins.insert(node_id);
                // Don't traverse past fan-in nodes
                continue;
            }

            auto edges = g.outgoing_edges(node_id);
            for (const auto* e : edges) {
                if (visited.find(e->to) == visited.end()) {
                    visited.insert(e->to);
                    bfs_queue.push(e->to);
                }
            }
        }
        per_branch_fan_ins.push_back(std::move(fan_ins));
    }

    if (per_branch_fan_ins.empty()) return "";

    // Intersect: find fan-in nodes reachable from ALL branches
    std::set<std::string> common = per_branch_fan_ins[0];
    for (size_t i = 1; i < per_branch_fan_ins.size(); ++i) {
        std::set<std::string> intersection;
        for (const auto& id : common) {
            if (per_branch_fan_ins[i].count(id)) {
                intersection.insert(id);
            }
        }
        common = std::move(intersection);
    }

    if (common.empty()) return "";
    if (common.size() == 1) return *common.begin();

    // Multiple common fan-ins: pick the nearest (shortest BFS distance from any branch)
    int best_dist = 999999;
    std::string best_id;
    for (const auto& fan_in_id : common) {
        for (const auto& start : branch_starts) {
            std::queue<std::pair<std::string, int>> bfs_q;
            std::set<std::string> vis;
            bfs_q.push({start, 0});
            vis.insert(start);
            while (!bfs_q.empty()) {
                auto cur = bfs_q.front(); bfs_q.pop();
                if (cur.first == fan_in_id) {
                    if (cur.second < best_dist ||
                        (cur.second == best_dist && fan_in_id < best_id)) {
                        best_dist = cur.second;
                        best_id = fan_in_id;
                    }
                    break;
                }
                auto edges = g.outgoing_edges(cur.first);
                for (const auto* e : edges) {
                    if (vis.find(e->to) == vis.end()) {
                        vis.insert(e->to);
                        bfs_q.push({e->to, cur.second + 1});
                    }
                }
            }
        }
    }
    return best_id;
}

std::set<std::string> SubgraphTopology::collect_loop_body(
    const Graph& g,
    const std::string& manager_id,
    const std::string& first_body_node)
{
    // BFS from first_body_node, stopping at manager_id (the loop-back target)
    std::set<std::string> stop_nodes;
    stop_nodes.insert(manager_id);
    return collect_reachable(g, first_body_node, stop_nodes);
}

} // namespace needle
