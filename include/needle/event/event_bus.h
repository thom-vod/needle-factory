#pragma once

#include <functional>
#include <vector>
#include <mutex>
#include "needle/event/event.h"

namespace needle {

typedef std::function<void(const PipelineEvent&)> EventCallback;

class EventBus {
public:
    void subscribe(EventCallback callback);
    void emit(const PipelineEvent& event);

private:
    std::mutex mutex_;
    std::vector<EventCallback> callbacks_;
};

} // namespace needle
