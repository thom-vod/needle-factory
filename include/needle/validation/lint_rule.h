#pragma once

#include <string>
#include "needle/model/graph.h"
#include "needle/validation/diagnostic.h"

namespace needle {

class LintRule {
public:
    virtual ~LintRule() {}
    virtual std::string name() const = 0;
    virtual void check(const Graph& graph, Diagnostics& diags) const = 0;
};

} // namespace needle
