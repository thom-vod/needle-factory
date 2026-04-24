#pragma once

#include <string>
#include "needle/model/result.h"
#include "needle/model/outcome.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"

namespace needle {

class Backend {
public:
    virtual ~Backend() {}
    virtual std::string name() const = 0;
    virtual Result<Outcome> execute(
        const Node& node,
        Context& ctx,
        const std::string& stage_dir
    ) = 0;
};

} // namespace needle
