#pragma once

#include "needle/validation/lint_rule.h"
#include "needle/parser/condition_parser.h"

namespace needle {

class ValidConditionsRule : public LintRule {
public:
    std::string name() const override { return "ValidConditions"; }

    void check(const Graph& graph, Diagnostics& diags) const override {
        for (const auto& edge : graph.edges()) {
            std::string cond = edge.condition();
            if (cond.empty()) continue;

            auto result = ConditionParser::parse(cond);
            if (!result.ok()) {
                Diagnostic d;
                d.severity = DiagnosticSeverity::ERROR;
                d.code = "E006";
                d.message = "Edge " + edge.from + " -> " + edge.to +
                            " has invalid condition '" + cond + "': " + result.error();
                diags.add(std::move(d));
            }
        }
    }
};

} // namespace needle
