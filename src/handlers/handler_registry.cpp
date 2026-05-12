#include "needle/handlers/handler_registry.h"
#include "needle/handlers/all_handlers.h"
#include "needle/handlers/interactive_session.h"
#include "needle/backend/backend.h"
#include "needle/backend/process_runner.h"
#include "needle/interviewer/interviewer.h"
#include "needle/engine/subgraph_executor.h"
#include "needle/worktree/strategy.h"

namespace needle {

void HandlerRegistry::register_handler(const std::string& type, std::shared_ptr<Handler> handler) {
    handlers_[type] = std::move(handler);
}

Handler* HandlerRegistry::get(const std::string& type) const {
    auto it = handlers_.find(type);
    if (it != handlers_.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool HandlerRegistry::has(const std::string& type) const {
    return handlers_.find(type) != handlers_.end();
}

std::shared_ptr<HandlerRegistry> HandlerRegistry::create_default(
    std::shared_ptr<Backend> cli_backend,
    std::shared_ptr<Backend> llmkit_backend,
    std::shared_ptr<Interviewer> interviewer,
    std::shared_ptr<SubgraphExecutor> subgraph_executor,
    std::shared_ptr<ProcessRunner> process_runner,
    std::shared_ptr<InteractiveSession> interactive_session)
{
    auto registry = std::make_shared<HandlerRegistry>();

    registry->register_handler("start", make_start_handler());
    registry->register_handler("exit", make_exit_handler());
    registry->register_handler("codergen", make_codergen_handler(cli_backend));
    registry->register_handler("llmkit", make_llmkit_handler(llmkit_backend));
    registry->register_handler("conditional", make_conditional_handler());
    registry->register_handler("parallel", make_parallel_handler(subgraph_executor, WorktreeConfig{}));
    registry->register_handler("fan_in", make_fan_in_handler(cli_backend));
    registry->register_handler("wait_human", make_wait_human_handler(interviewer));
    registry->register_handler("tool", make_tool_handler(process_runner));
    registry->register_handler("manager_loop", make_manager_loop_handler(subgraph_executor));
    registry->register_handler("web_search", make_web_search_handler(process_runner));
    registry->register_handler("doc_fetch", make_doc_fetch_handler(process_runner));
    registry->register_handler("nested_run", make_nested_run_handler(process_runner));
    registry->register_handler("interactive", make_interactive_handler(cli_backend, interactive_session));

    return registry;
}

} // namespace needle
