#pragma once

#include "needle/handlers/handler.h"
#include "needle/util/logger.h"

namespace needle {

class HandlerBase : public Handler {
public:
    virtual ~HandlerBase() {}
    std::string type_name() const override = 0;

    // Final: wraps do_execute with logging, exception handling, contract checks.
    Result<Outcome> execute(const Node& node, Context& ctx,
                            const ExecutionContext& exec_ctx) override final;

protected:
    virtual Result<Outcome> do_execute(const Node& node, Context& ctx,
                                       const ExecutionContext& exec_ctx) = 0;

    void emit_warning(const ExecutionContext& exec_ctx,
                      const std::string& node_id,
                      const std::string& message);

    void write_stage_file(const ExecutionContext& exec_ctx,
                          const std::string& node_id,
                          const std::string& filename,
                          const std::string& content);

    static Outcome make_failure(const std::string& error_message);
    static Outcome make_success(const std::string& output = "");
    static Outcome make_skip(const std::string& reason);
};

} // namespace needle
