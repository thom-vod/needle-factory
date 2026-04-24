#pragma once

#include <string>
#include <map>
#include <memory>
#include "needle/handlers/handler.h"

namespace needle {

class Backend;
class Interviewer;
class SubgraphExecutor;
class ProcessRunner;
struct InteractiveSession;

class HandlerRegistry {
public:
    void register_handler(const std::string& type, std::shared_ptr<Handler> handler);
    Handler* get(const std::string& type) const;
    bool has(const std::string& type) const;

    // Register all built-in handlers
    static std::shared_ptr<HandlerRegistry> create_default(
        std::shared_ptr<Backend> cli_backend,
        std::shared_ptr<Backend> llmkit_backend,
        std::shared_ptr<Interviewer> interviewer,
        std::shared_ptr<SubgraphExecutor> subgraph_executor,
        std::shared_ptr<ProcessRunner> process_runner = nullptr,
        std::shared_ptr<InteractiveSession> interactive_session = nullptr
    );

private:
    std::map<std::string, std::shared_ptr<Handler>> handlers_;
};

} // namespace needle
