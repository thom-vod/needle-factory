#pragma once

#include <string>
#include <atomic>
#include "needle/model/graph.h"
#include "needle/model/fidelity.h"
#include "needle/event/event_bus.h"

namespace needle {

struct ExecutionContext {
    const Graph& graph;
    EventBus& event_bus;
    const std::string& logs_root;
    const std::string& project_dir;
    FidelityMode default_fidelity;
    std::atomic<bool>& cancelled;
};

} // namespace needle
