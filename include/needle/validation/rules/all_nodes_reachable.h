#pragma once

#include "needle/validation/lint_rule.h"
#include <set>
#include <queue>

namespace needle {

class AllNodesReachableRule : public LintRule {
public:
    std::string name() const override { return "AllNodesReachable"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        const Node* start = graph.start_node();
        if (!start) return; // E001 will catch this

        // BFS from start
        std::set<std::string> visited;
        std::queue<std::string> queue;
        queue.push(start->id);
        visited.insert(start->id);

        while (!queue.empty()) {
            std::string current = queue.front();
            queue.pop();

            auto edges = graph.outgoing_edges(current);
            for (const auto* edge : edges) {
                if (visited.find(edge->to) == visited.end()) {
                    visited.insert(edge->to);
                    queue.push(edge->to);
                }
            }
        }

        for (const auto& node : graph.nodes()) {
            if (visited.find(node.id) == visited.end()) {
                Diagnostic d;
                d.severity = DiagnosticSeverity::ERROR;
                d.code = "E005";
                d.message = "Node '" + node.id + "' is not reachable from START";
                d.node_id = node.id;
                diags.add(std::move(d));
            }
        }
    }
};

} // namespace needle
