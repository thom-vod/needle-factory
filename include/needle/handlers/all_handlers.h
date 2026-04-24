#pragma once

// Forward factory declarations for all handler types.
// Each returns a shared_ptr<Handler> so the registry can wire them up
// without exposing the concrete classes.

#include <memory>
#include "needle/handlers/handler.h"

namespace needle {

class Backend;
class ProcessRunner;
class Interviewer;
class SubgraphExecutor;
struct InteractiveSession;

std::shared_ptr<Handler> make_start_handler();
std::shared_ptr<Handler> make_exit_handler();
std::shared_ptr<Handler> make_codergen_handler(std::shared_ptr<Backend> backend);
std::shared_ptr<Handler> make_llmkit_handler(std::shared_ptr<Backend> backend);
std::shared_ptr<Handler> make_conditional_handler();
std::shared_ptr<Handler> make_parallel_handler(std::shared_ptr<SubgraphExecutor> executor);
std::shared_ptr<Handler> make_fan_in_handler(std::shared_ptr<Backend> backend = nullptr);
std::shared_ptr<Handler> make_wait_human_handler(std::shared_ptr<Interviewer> interviewer);
std::shared_ptr<Handler> make_tool_handler(std::shared_ptr<ProcessRunner> runner);
std::shared_ptr<Handler> make_manager_loop_handler(std::shared_ptr<SubgraphExecutor> executor);
std::shared_ptr<Handler> make_web_search_handler(std::shared_ptr<ProcessRunner> runner);
std::shared_ptr<Handler> make_doc_fetch_handler(std::shared_ptr<ProcessRunner> runner);
std::shared_ptr<Handler> make_nested_run_handler(std::shared_ptr<ProcessRunner> runner);
std::shared_ptr<Handler> make_interactive_handler(std::shared_ptr<Backend> backend,
                                                  std::shared_ptr<InteractiveSession> session = nullptr);

} // namespace needle
