#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>

namespace needle {

// Global pause controller — shared between server and all engine threads.
// When paused, engine threads block before starting new nodes.
struct PauseController {
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool paused = false;
    std::string resume_at;  // ISO 8601 timestamp for scheduled resume, empty = no schedule

    // Block until unpaused (or cancelled). Returns true if unpaused, false if cancelled.
    bool wait_if_paused(std::atomic<bool>& cancelled) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return !paused || cancelled.load(); });
        return !cancelled.load();
    }

    void pause() {
        std::lock_guard<std::mutex> lock(mutex);
        paused = true;
        resume_at.clear();
    }

    void resume() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            paused = false;
            resume_at.clear();
        }
        cv.notify_all();
    }

    void schedule_resume(const std::string& iso_time) {
        std::lock_guard<std::mutex> lock(mutex);
        resume_at = iso_time;
    }

    bool is_paused() const {
        std::lock_guard<std::mutex> lock(mutex);
        return paused;
    }

    std::string get_resume_at() const {
        std::lock_guard<std::mutex> lock(mutex);
        return resume_at;
    }
};

} // namespace needle
