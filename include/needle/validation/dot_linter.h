#pragma once

#include <string>
#include <vector>
#include <map>

#include "needle/model/graph.h"
#include "needle/model/context.h"

namespace needle {

struct LintWarning {
    std::string code;
    std::string node_id;
    std::string message;
    int line = 0;
    std::string severity = "warning";
};

class DotLinter {
public:
    std::vector<LintWarning> lint(const Graph& graph, const std::map<std::string, std::string>& cli_vars) const;
};

} // namespace needle
