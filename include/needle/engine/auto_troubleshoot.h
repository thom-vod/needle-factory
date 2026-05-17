#pragma once

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
    Escalated,
};

struct AutoTroubleshootResult {
    AutoTroubleshootAction action = AutoTroubleshootAction::Skipped;
    std::string session_id;
    std::string report_path;
    std::string message;
};

class AutoTroubleshoot {
public:
    explicit AutoTroubleshoot(std::shared_ptr<ProcessRunner> runner = nullptr,
                              std::shared_ptr<InteractiveSession> interactive_session = nullptr);

    AutoTroubleshootResult handle(const std::string& node_id,
                                  const Graph& graph,
                                  const std::string& run_dir,
                                  Context& ctx,
                                  int max_attempts_per_stage,
                                  TroubleshootMode mode,
                                  EventBus* event_bus = nullptr);

    AutoTroubleshootResult handle(const std::string& node_id,
                                  const Graph& graph,
                                  const std::string& run_dir,
                                  Context& ctx,
                                  int max_attempts_per_stage,
                                  TroubleshootMode mode,
                                  TroubleshootTrust trust,
                                  EventBus* event_bus);

private:
    std::shared_ptr<ProcessRunner> runner_;
    std::shared_ptr<InteractiveSession> interactive_session_;
};

} // namespace needle
