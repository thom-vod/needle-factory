#pragma once

#include <functional>
#include <memory>
#include <string>

#include "needle/backend/process_runner.h"
#include "needle/event/event_bus.h"
#include "needle/handlers/interactive_session.h"
#include "needle/model/context.h"
#include "needle/model/graph.h"
#include "needle/troubleshoot/types.h"

namespace needle {

enum class AutoTroubleshootAction {
    Skipped,
    Resumed,
    Reported,
    Escalated,
    Cancelled,
};

struct AutoTroubleshootResult {
    AutoTroubleshootAction action = AutoTroubleshootAction::Skipped;
    std::string session_id;
    std::string report_path;
    std::string message;
};

class AutoTroubleshoot {
public:
    // Optional: called once per session BEFORE the agent process is
    // spawned. The shared_ptr is the per-session NativeProcessRunner
    // (or any ProcessRunner) that owns the agent's child process,
    // so an external cancel path can find and kill it. Pass nullptr
    // (the default) when no external cancel surface exists (CLI).
    using RegisterRunnerFn = std::function<void(const std::string& run_id,
                                                const std::string& session_id,
                                                std::shared_ptr<ProcessRunner>)>;
    using UnregisterRunnerFn = std::function<void(const std::string& run_id,
                                                  const std::string& session_id)>;

    explicit AutoTroubleshoot(std::shared_ptr<ProcessRunner> runner = nullptr);
    void set_register_runner(RegisterRunnerFn fn);
    void set_unregister_runner(UnregisterRunnerFn fn);

    AutoTroubleshootResult handle(const std::string& node_id,
                                  const Graph& graph,
                                  const std::string& run_dir,
                                  Context& ctx,
                                  int max_attempts_per_stage,
                                  TroubleshootMode mode,
                                  EventBus* event_bus = nullptr);

private:
    std::shared_ptr<ProcessRunner> runner_;
    RegisterRunnerFn register_runner_;
    UnregisterRunnerFn unregister_runner_;
};

} // namespace needle
