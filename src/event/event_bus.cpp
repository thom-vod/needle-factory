#include "needle/event/event_bus.h"

namespace needle {

void EventBus::subscribe(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(std::move(callback));
}

void EventBus::emit(const PipelineEvent& event) {
    std::vector<EventCallback> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_copy = callbacks_;
    }
    for (const auto& cb : callbacks_copy) {
        cb(event);
    }
}

} // namespace needle
