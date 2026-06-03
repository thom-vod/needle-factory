#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "needle/config/needle_config.h"
#include "needle/platform/platform.h"

// Portable setenv: setenv() is POSIX-only; Windows (MinGW/MSVC) uses
// _putenv_s. Mirrors the helper in tests/support/temp_needle_state.h.
#ifdef _WIN32
#include <process.h>
static inline int needle_test_setenv(const char* name, const char* value) {
    return _putenv_s(name, value);
}
#define NEEDLE_TEST_GETPID _getpid
#else
#include <unistd.h>
static inline int needle_test_setenv(const char* name, const char* value) {
    return setenv(name, value, 1);
}
#define NEEDLE_TEST_GETPID getpid
#endif

// Sandbox the config path so tests don't write to the operator's real
// ~/.needle/config.json. Several tests call NeedleConfig::global().set(...)
// which writes the (sparse) singleton state to disk via save_impl();
// without sandboxing, every `needle_tests` invocation overwrites the
// operator's config with whatever sparse blob the test set up.
//
// Also set NEEDLE_ALLOW_SPARSE_CONFIG=1 so the production safety guard
// in save_impl() doesn't refuse sparse-state saves inside tests.
int main(int argc, char* argv[]) {
    needle_test_setenv("NEEDLE_ALLOW_SPARSE_CONFIG", "1");

    // Cross-platform sandbox dir under the system temp directory.
    // (mkdtemp() is POSIX-only, so build a unique path by hand and mkdir it.)
    std::string sandbox_dir = needle::platform::path_join(
        needle::platform::temp_dir(),
        "needle-tests-config-" + std::to_string(NEEDLE_TEST_GETPID()));
    std::string sandbox_path;
    if (needle::platform::mkdir_p(sandbox_dir)) {
        sandbox_path = needle::platform::path_join(sandbox_dir, "config.json");
        needle::NeedleConfig::global().set_config_path(sandbox_path);
    }

    int result = Catch::Session().run(argc, argv);

    if (!sandbox_dir.empty()) {
        needle::platform::remove_recursive(sandbox_dir);
    }
    return result;
}
