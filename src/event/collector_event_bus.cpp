#include "needle/event/collector_event_bus.h"

namespace needle {

void CollectorEventBus::record(const PipelineEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
}

std::vector<PipelineEvent> CollectorEventBus::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

size_t CollectorEventBus::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.size();
}

void CollectorEventBus::replay_to(EventCallback callback) const {
    std::vector<PipelineEvent> events_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_copy = events_;
    }
    for (const auto& event : events_copy) {
        callback(event);
    }
}

} // namespace needle
