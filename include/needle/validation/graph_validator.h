#pragma once

#include <vector>
#include <memory>
#include "needle/validation/lint_rule.h"
#include "needle/validation/diagnostic.h"
#include "needle/model/graph.h"

namespace needle {

class GraphValidator {
public:
    explicit GraphValidator(std::vector<std::shared_ptr<LintRule>> rules);

    Diagnostics validate(const Graph& graph) const;

    static GraphValidator create_default();

private:
    std::vector<std::shared_ptr<LintRule>> rules_;
};

} // namespace needle
