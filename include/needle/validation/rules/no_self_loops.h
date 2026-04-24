#pragma once

#include "needle/validation/lint_rule.h"

namespace needle {

class NoSelfLoopsRule : public LintRule {
public:
    std::string name() const override { return "NoSelfLoops"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        for (const auto& edge : graph.edges()) {
            if (edge.from == edge.to) {
                Diagnostic d;
                d.severity = DiagnosticSeverity::WARNING;
                d.code = "W001";
                d.message = "Self-loop detected on node '" + edge.from + "'";
                d.node_id = edge.from;
                diags.add(std::move(d));
            }
        }
    }
};

} // namespace needle
