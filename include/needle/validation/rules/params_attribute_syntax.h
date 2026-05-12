#pragma once

#include "needle/validation/lint_rule.h"

namespace needle {

class ParamsAttributeSyntaxRule : public LintRule {
public:
    std::string name() const override { return "ParamsAttributeSyntax"; }
    void check(const Graph& graph, Diagnostics& diags) const override;
};

} // namespace needle
