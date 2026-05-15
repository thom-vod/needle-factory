#pragma once

#include "needle/platform/platform.h"
#include "needle/util/run_registry.h"

#include <cstdlib>
#include <memory>
#include <string>

#ifdef _WIN32
#include <process.h>
static inline int needle_test_setenv(const char* name, const char* value) {
    return _putenv_s(name, value);
}
static inline int needle_test_unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#define NEEDLE_TEST_GETPID _getpid
#else
#include <unistd.h>
static inline int needle_test_setenv(const char* name, const char* value) {
    return setenv(name, value, 1);
}
static inline int needle_test_unsetenv(const char* name) {
    return unsetenv(name);
}
#define NEEDLE_TEST_GETPID getpid
#endif

namespace needle {
namespace tests {

class TempNeedleState {
public:
    TempNeedleState()
        : root_(platform::path_join(platform::temp_dir(), unique_name()))
        , registry_path_(platform::path_join(root_, "runs.json"))
    {
        const char* prior = std::getenv("NEEDLE_RUNS_PATH");
        had_prior_runs_path_ = prior != nullptr;
        if (prior) prior_runs_path_ = prior;

        platform::mkdir_p(root_);
        needle_test_setenv("NEEDLE_RUNS_PATH", registry_path_.c_str());
    }

    TempNeedleState(const TempNeedleState&) = delete;
    TempNeedleState& operator=(const TempNeedleState&) = delete;

    ~TempNeedleState() {
        if (had_prior_runs_path_) {
            needle_test_setenv("NEEDLE_RUNS_PATH", prior_runs_path_.c_str());
        } else {
            needle_test_unsetenv("NEEDLE_RUNS_PATH");
        }
        platform::remove_recursive(root_);
    }

    const std::string& root() const { return root_; }
    const std::string& registry_path() const { return registry_path_; }

    std::string path(const std::string& name) const {
        return platform::path_join(root_, name);
    }

    std::unique_ptr<RunRegistry> make_registry() const {
        return std::unique_ptr<RunRegistry>(new RunRegistry(registry_path_));
    }

private:
    static std::string unique_name() {
        static int counter = 0;
        return "needle_state_test_" + std::to_string(NEEDLE_TEST_GETPID()) + "_" +
               std::to_string(counter++);
    }

    std::string root_;
    std::string registry_path_;
    bool had_prior_runs_path_ = false;
    std::string prior_runs_path_;
};

} // namespace tests
} // namespace needle
