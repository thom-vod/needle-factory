#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "needle/config/needle_config.h"

// Sandbox the config path so tests don't write to the operator's real
// ~/.needle/config.json. Several tests call NeedleConfig::global().set(...)
// which writes the (sparse) singleton state to disk via save_impl();
// without sandboxing, every `needle_tests` invocation overwrites the
// operator's config with whatever sparse blob the test set up.
//
// Also set NEEDLE_ALLOW_SPARSE_CONFIG=1 so the production safety guard
// in save_impl() doesn't refuse sparse-state saves inside tests.
int main(int argc, char* argv[]) {
    ::setenv("NEEDLE_ALLOW_SPARSE_CONFIG", "1", 1);

    char tmpl[] = "/tmp/needle-tests-config-XXXXXX";
    char* tmpdir = ::mkdtemp(tmpl);
    std::string sandbox_path;
    if (tmpdir) {
        sandbox_path = std::string(tmpdir) + "/config.json";
        needle::NeedleConfig::global().set_config_path(sandbox_path);
    }

    int result = Catch::Session().run(argc, argv);

    if (!sandbox_path.empty()) {
        std::remove(sandbox_path.c_str());
        if (tmpdir) std::remove(tmpdir);
    }
    return result;
}
