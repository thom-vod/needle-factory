#pragma once

#include "needle/validation/lint_rule.h"

namespace needle {

class SingleStartNodeRule : public LintRule {
public:
    std::string name() const override { return "SingleStartNode"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        int count = 0;
        for (const auto& node : graph.nodes()) {
            if (node.type == NodeType::START) {
                ++count;
            }
        }
        if (count == 0) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.code = "E001";
            d.message = "Graph has no START node (Mdiamond shape required)";
            diags.add(std::move(d));
        } else if (count > 1) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.code = "E001";
            d.message = "Graph has " + std::to_string(count) + " START nodes, expected exactly 1";
            diags.add(std::move(d));
        }
    }
};

} // namespace needle
