#include "needle/engine/run_guard.h"

#include <map>
#include <mutex>

#include "needle/util/logger.h"

namespace needle {

namespace {

std::mutex& map_mutex() {
    static std::mutex m;
    return m;
}

std::map<std::string, bool>& active_runs() {
    static std::map<std::string, bool> m;
    return m;
}

} // namespace

GuardReleaser::GuardReleaser(std::string run_id)
    : run_id_(std::move(run_id)) {}

GuardReleaser::~GuardReleaser() {
    release_if_acquired();
}

GuardReleaser::GuardReleaser(GuardReleaser&& other) noexcept
    : run_id_(std::move(other.run_id_))
    , acquired_(other.acquired_) {
    other.acquired_ = false;
}

GuardReleaser& GuardReleaser::operator=(GuardReleaser&& other) noexcept {
    if (this != &other) {
        release_if_acquired();
        run_id_ = std::move(other.run_id_);
        acquired_ = other.acquired_;
        other.acquired_ = false;
    }
    return *this;
}

void GuardReleaser::release_if_acquired() {
    if (acquired_) {
        RunGuard::release(run_id_);
        acquired_ = false;
    }
}

bool RunGuard::try_acquire(const std::string& run_id) {
    if (run_id.empty()) {
        NEEDLE_LOG_WARN("troubleshoot", "RunGuard bypassed: empty run_id");
        return true; // tests / fixtures without a run_id
    }
    std::lock_guard<std::mutex> lock(map_mutex());
    auto& m = active_runs();
    auto it = m.find(run_id);
    if (it != m.end() && it->second) return false;
    m[run_id] = true;
    return true;
}

void RunGuard::release(const std::string& run_id) {
    if (run_id.empty()) return;
    std::lock_guard<std::mutex> lock(map_mutex());
    active_runs().erase(run_id);
}

std::optional<GuardReleaser> RunGuard::try_reserve(const std::string& run_id) {
    if (!try_acquire(run_id)) return std::nullopt;
    return GuardReleaser(run_id);
}

} // namespace needle
