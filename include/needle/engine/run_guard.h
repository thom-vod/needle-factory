#pragma once

#include <optional>
#include <string>

namespace needle {

class GuardReleaser {
public:
    explicit GuardReleaser(std::string run_id);
    ~GuardReleaser();

    GuardReleaser(const GuardReleaser&) = delete;
    GuardReleaser& operator=(const GuardReleaser&) = delete;
    GuardReleaser(GuardReleaser&& other) noexcept;
    GuardReleaser& operator=(GuardReleaser&& other) noexcept;

private:
    void release_if_acquired();

    std::string run_id_;
    bool acquired_ = true;
};

class RunGuard {
public:
    static bool try_acquire(const std::string& run_id);
    static void release(const std::string& run_id);
    static std::optional<GuardReleaser> try_reserve(const std::string& run_id);
};

} // namespace needle
