#pragma once

#include <string>
#include "needle/model/result.h"
#include "needle/model/outcome.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/engine/execution_context.h"

namespace needle {

class Handler {
public:
    virtual ~Handler() {}
    virtual std::string type_name() const = 0;
    virtual Result<Outcome> execute(
        const Node& node,
        Context& ctx,
        const ExecutionContext& exec_ctx
    ) = 0;
};

} // namespace needle
