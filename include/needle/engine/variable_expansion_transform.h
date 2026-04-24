#pragma once

#include "needle/engine/transform.h"
#include <vector>
#include <utility>

namespace needle {

// Concrete transform that expands $var.X, ${context.key}, $goal references
// in node attributes. After apply() is called, unresolved_vars() returns
// any variables that could not be resolved.
class VariableExpansionTransform : public Transform {
public:
    std::string name() const override;
    Result<void> apply(Graph& graph, const Context& ctx) const override;

    // Returns (node_id, variable_name) pairs for variables that were
    // referenced but could not be resolved. Only valid after apply().
    std::vector<std::pair<std::string, std::string>> unresolved_vars() const;

private:
    mutable std::vector<std::pair<std::string, std::string>> unresolved_;
};

// Factory (returns base Transform pointer for generic use)
std::shared_ptr<VariableExpansionTransform> make_typed_variable_expansion_transform();

} // namespace needle
