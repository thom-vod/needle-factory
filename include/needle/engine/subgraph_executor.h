#pragma once

#include <string>
#include "needle/model/result.h"
#include "needle/model/outcome.h"
#include "needle/model/context.h"

namespace needle {

struct ExecutionContext;
class RetryController;

class SubgraphExecutor {
public:
    virtual ~SubgraphExecutor() {}
    virtual Result<Outcome> execute_subgraph(
        const std::string& start_node_id,
        const std::string& end_node_id,
        Context& ctx,
        const ExecutionContext& exec_ctx
    ) = 0;

    // M7: Overload accepting a per-branch RetryController (for parallel branches)
    virtual Result<Outcome> execute_subgraph(
        const std::string& start_node_id,
        const std::string& end_node_id,
        Context& ctx,
        const ExecutionContext& exec_ctx,
        RetryController* branch_retry
    ) {
        // Default: ignore branch_retry, use the original method
        (void)branch_retry;
        return execute_subgraph(start_node_id, end_node_id, ctx, exec_ctx);
    }

    // When inclusive_end=false the end node is NOT executed (subgraph stops
    // at its predecessor). Used by parallel branches so the fan-in runs once
    // in the parent context, not once per branch before parallel state is set.
    virtual Result<Outcome> execute_subgraph(
        const std::string& start_node_id,
        const std::string& end_node_id,
        Context& ctx,
        const ExecutionContext& exec_ctx,
        RetryController* branch_retry,
        bool inclusive_end
    ) {
        // Default for implementations that don't opt in: fall back to inclusive.
        (void)inclusive_end;
        return execute_subgraph(start_node_id, end_node_id, ctx, exec_ctx, branch_retry);
    }
};

} // namespace needle
