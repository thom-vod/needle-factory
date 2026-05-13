#pragma once

#include "needle/validation/lint_rule.h"

namespace needle {

class StartNoIncomingRule : public LintRule {
public:
    std::string name() const override { return "StartNoIncoming"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        const Node* start = graph.start_node();
        if (!start) return; // E001 will catch this

        auto incoming = graph.incoming_edges(start->id);
        if (!incoming.empty()) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.code = "E003";
            d.message = "START node '" + start->id + "' has " +
                        std::to_string(incoming.size()) + " incoming edge(s)";
            d.node_id = start->id;
            diags.add(std::move(d));
        }
    }
};

} // namespace needle
