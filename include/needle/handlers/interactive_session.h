#pragma once

#include <string>
#include <mutex>
#include <condition_variable>

namespace needle {

struct InteractiveSession {
    std::mutex mutex;
    std::condition_variable cv;
    bool active = false;
    bool continued = false;
    bool go_back = false;
    std::string node_id;
    std::string prompt;
    std::string context_summary;
    std::string pipeline_context;   // describes position in pipeline (name, step, next, prev)
    std::string previous_node_id;   // for go-back routing
    std::string final_result;
};

} // namespace needle
