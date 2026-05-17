#pragma once

#include <string>
#include <mutex>
#include <condition_variable>
#include <map>
#include <memory>

namespace needle {

struct InteractiveSession {
    explicit InteractiveSession(const std::string& opener_message = "")
        : opener(opener_message) {}

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
    std::string opener;             // optional assistant message persisted as first chat turn
};

class InteractiveSessionRegistry {
public:
    static void register_session(const std::string& node_id,
                                 std::shared_ptr<InteractiveSession> session) {
        std::lock_guard<std::mutex> lock(mutex());
        sessions()[node_id] = std::move(session);
    }

    static std::shared_ptr<InteractiveSession> get(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = sessions().find(node_id);
        return it == sessions().end() ? nullptr : it->second;
    }

    static void clear() {
        std::lock_guard<std::mutex> lock(mutex());
        sessions().clear();
    }

private:
    static std::map<std::string, std::shared_ptr<InteractiveSession>>& sessions() {
        static std::map<std::string, std::shared_ptr<InteractiveSession>> value;
        return value;
    }

    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }
};

} // namespace needle
