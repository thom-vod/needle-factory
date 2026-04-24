#pragma once

#include "needle/validation/lint_rule.h"

namespace needle {

class ExitNoOutgoingRule : public LintRule {
public:
    std::string name() const override { return "ExitNoOutgoing"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        const Node* exit = graph.exit_node();
        if (!exit) return; // E002 will catch this

        auto outgoing = graph.outgoing_edges(exit->id);
        if (!outgoing.empty()) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::ERROR;
            d.code = "E004";
            d.message = "EXIT node '" + exit->id + "' has " +
                        std::to_string(outgoing.size()) + " outgoing edge(s)";
            d.node_id = exit->id;
            diags.add(std::move(d));
        }
    }
};

} // namespace needle
