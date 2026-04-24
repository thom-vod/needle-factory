#pragma once

#include "needle/validation/lint_rule.h"

namespace needle {

class SingleExitNodeRule : public LintRule {
public:
    std::string name() const override { return "SingleExitNode"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        int count = 0;
        for (const auto& node : graph.nodes()) {
            if (node.type == NodeType::EXIT) {
                ++count;
            }
        }
        if (count == 0) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::ERROR;
            d.code = "E002";
            d.message = "Graph has no EXIT node (Msquare shape required)";
            diags.add(std::move(d));
        } else if (count > 1) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::ERROR;
            d.code = "E002";
            d.message = "Graph has " + std::to_string(count) + " EXIT nodes, expected exactly 1";
            diags.add(std::move(d));
        }
    }
};

} // namespace needle
