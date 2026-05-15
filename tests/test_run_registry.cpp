#include <catch2/catch.hpp>

#include "needle/platform/platform.h"
#include "needle/util/run_registry.h"
#include "support/temp_needle_state.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

using namespace needle;

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name)
    {
        const char* prior = std::getenv(name);
        had_prior_ = prior != nullptr;
        if (prior) prior_ = prior;
        needle_test_setenv(name, value.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

    ~ScopedEnvVar() {
        if (had_prior_) {
            needle_test_setenv(name_.c_str(), prior_.c_str());
        } else {
            needle_test_unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    bool had_prior_ = false;
    std::string prior_;
};

void add_sample_entry(RunRegistry& registry) {
    registry.add_entry("run-test", "test", "digraph {}", "/tmp/project",
                       "/tmp/logs", "running", "2026-05-15T00:00:00Z");
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    REQUIRE(out.is_open());
    out << content;
}

} // namespace

TEST_CASE("RunRegistry default path honors NEEDLE_RUNS_PATH", "[run_registry]") {
    tests::TempNeedleState state;

    REQUIRE(RunRegistry::default_registry_path() == state.registry_path());

    RunRegistry registry;
    REQUIRE(registry.load().ok());
    add_sample_entry(registry);
    REQUIRE(registry.save().ok());

    REQUIRE(platform::file_exists(state.registry_path()));
}

TEST_CASE("RunRegistry save creates parent directories for explicit paths", "[run_registry]") {
    tests::TempNeedleState state;
    std::string registry_path = state.path("nested/registry/runs.json");

    RunRegistry registry(registry_path);
    REQUIRE(registry.load().ok());
    add_sample_entry(registry);
    REQUIRE(registry.save().ok());

    REQUIRE(platform::file_exists(registry_path));
}

TEST_CASE("RunRegistry default operations do not touch home runs.json when scratch env is set",
          "[run_registry]") {
    tests::TempNeedleState state;
    std::string fake_home = state.path("home");
    std::string needle_dir = platform::path_join(fake_home, ".needle");
    std::string home_registry = platform::path_join(needle_dir, "runs.json");
    platform::mkdir_p(needle_dir);

    const std::string sentinel = "{\"sentinel\":true}\n";
    {
        std::ofstream out(home_registry);
        out << sentinel;
    }

    ScopedEnvVar home("HOME", fake_home);
    ScopedEnvVar userprofile("USERPROFILE", fake_home);

    RunRegistry registry;
    REQUIRE(registry.load().ok());
    add_sample_entry(registry);
    REQUIRE(registry.save().ok());

    REQUIRE(platform::file_exists(state.registry_path()));
    REQUIRE(platform::file_exists(home_registry));
    REQUIRE(read_file(home_registry) == sentinel);
}

TEST_CASE("RunRegistry persists dry_run flag and defaults missing entries to false",
          "[run_registry]") {
    tests::TempNeedleState state;

    RunRegistry registry(state.registry_path());
    REQUIRE(registry.load().ok());
    registry.add_entry("run-dry", "test", "digraph {}", "/tmp/project",
                       "/tmp/logs-dryrun", "completed", "2026-05-15T00:00:00Z",
                       true);
    REQUIRE(registry.save().ok());

    RunRegistry reloaded(state.registry_path());
    REQUIRE(reloaded.load().ok());
    auto runs = reloaded.all();
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].value("dry_run", false));

    std::string legacy_path = state.path("legacy-runs.json");
    write_file(legacy_path,
               "{\n"
               "  \"version\": 1,\n"
               "  \"runs\": {\n"
               "    \"legacy\": {\"id\":\"legacy\"}\n"
               "  }\n"
               "}\n");
    RunRegistry legacy(legacy_path);
    REQUIRE(legacy.load().ok());
    auto legacy_runs = legacy.all();
    REQUIRE(legacy_runs.size() == 1);
    REQUIRE(legacy_runs[0].contains("dry_run"));
    REQUIRE_FALSE(legacy_runs[0]["dry_run"].get<bool>());
}
