#pragma once

#include <optional>
#include <string>

namespace needle {

class RunGuard;

class GuardReleaser {
public:
    ~GuardReleaser();

    GuardReleaser(const GuardReleaser&) = delete;
    GuardReleaser& operator=(const GuardReleaser&) = delete;
    GuardReleaser(GuardReleaser&& other) noexcept;
    GuardReleaser& operator=(GuardReleaser&& other) noexcept;

private:
    friend class RunGuard;
    // Only RunGuard::try_reserve constructs releasers — preserves the
    // invariant that a live releaser always corresponds to an actually
    // acquired slot.
    explicit GuardReleaser(std::string run_id);
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
