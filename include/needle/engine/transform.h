#pragma once

#include <string>
#include <memory>
#include "needle/model/result.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/parser/stylesheet_parser.h"

namespace needle {

class Transform {
public:
    virtual ~Transform() {}
    virtual std::string name() const = 0;
    virtual Result<void> apply(Graph& graph, const Context& ctx) const = 0;
};

// Factory functions for built-in transforms
std::shared_ptr<Transform> make_stylesheet_transform(Stylesheet stylesheet);
std::shared_ptr<Transform> make_variable_expansion_transform();

} // namespace needle
