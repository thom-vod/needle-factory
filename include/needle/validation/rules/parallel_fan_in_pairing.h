#pragma once

#include "needle/validation/lint_rule.h"
#include <set>
#include <queue>

namespace needle {

class ParallelFanInPairingRule : public LintRule {
public:
    std::string name() const override { return "ParallelFanInPairing"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        for (const auto& node : graph.nodes()) {
            if (node.type != NodeType::PARALLEL) continue;

            // BFS forward from this PARALLEL node to find a FAN_IN node
            bool found_fan_in = false;
            std::set<std::string> visited;
            std::queue<std::string> queue;

            // Start from outgoing neighbors (not the PARALLEL node itself)
            auto out_edges = graph.outgoing_edges(node.id);
            for (const auto* edge : out_edges) {
                if (visited.find(edge->to) == visited.end()) {
                    visited.insert(edge->to);
                    queue.push(edge->to);
                }
            }

            while (!queue.empty() && !found_fan_in) {
                std::string current = queue.front();
                queue.pop();

                const Node* n = graph.find_node(current);
                if (n && n->type == NodeType::FAN_IN) {
                    found_fan_in = true;
                    break;
                }

                auto edges = graph.outgoing_edges(current);
                for (const auto* edge : edges) {
                    if (visited.find(edge->to) == visited.end()) {
                        visited.insert(edge->to);
                        queue.push(edge->to);
                    }
                }
            }

            if (!found_fan_in) {
                Diagnostic d;
                d.severity = DiagnosticSeverity::ERROR;
                d.code = "E007";
                d.message = "PARALLEL node '" + node.id +
                            "' has no reachable FAN_IN node";
                d.node_id = node.id;
                diags.add(std::move(d));
            }
        }
    }
};

} // namespace needle
