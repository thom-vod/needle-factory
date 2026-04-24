#pragma once

#include <vector>
#include <mutex>
#include "needle/event/event.h"
#include "needle/event/event_bus.h"

namespace needle {

class CollectorEventBus {
public:
    void record(const PipelineEvent& event);
    std::vector<PipelineEvent> events() const;
    size_t size() const;
    void replay_to(EventCallback callback) const;

private:
    mutable std::mutex mutex_;
    std::vector<PipelineEvent> events_;
};

} // namespace needle
