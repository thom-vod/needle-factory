#include <catch2/catch.hpp>

#include "needle/engine/troubleshoot_snapshot.h"
#include "needle/platform/platform.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace needle;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const std::string& path, const std::string& value) {
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) platform::mkdir_p(path.substr(0, slash));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value;
}

struct SnapshotFixture {
    std::string root;
    std::string project;
    std::string graph;
    std::string run_dir;
    std::string session_dir;
    std::string config;
    std::string old_home;
    bool had_home = false;

    SnapshotFixture() {
        const char* home = std::getenv("HOME");
        if (home) {
            had_home = true;
            old_home = home;
        }
        root = platform::temp_dir() + "/needle_snapshot_test";
        platform::remove_recursive(root);
        project = root + "/project";
        graph = project + "/flow.dot";
        run_dir = project + "/.needle/flow";
        session_dir = run_dir + "/troubleshoot/session-test";
        config = root + "/home/.needle/config.json";
        write_file(graph, "digraph flow { a; }\n");
        write_file(project + "/.needle/flow/stages/a/prompt.md", "prompt before\n");
        write_file(config, "{\"defaults\":{\"x\":\"before\"}}\n");
        platform::mkdir_p(session_dir + "/snapshot");
        setenv("HOME", (root + "/home").c_str(), 1);
    }

    ~SnapshotFixture() {
        if (had_home) setenv("HOME", old_home.c_str(), 1);
        else unsetenv("HOME");
        platform::remove_recursive(root);
    }
};

} // namespace

TEST_CASE("TroubleshootSnapshot captures and restores graph prompts and config",
          "[troubleshoot][snapshot]") {
    SnapshotFixture f;

    auto captured = TroubleshootSnapshot::capture(f.project, f.graph, f.session_dir,
                                                  TroubleshootMode::Tweak);
    REQUIRE(captured.ok());
    REQUIRE(platform::file_exists(f.session_dir + "/snapshot/flow.dot"));
    REQUIRE(platform::file_exists(f.session_dir + "/snapshot/prompt.md.a"));
    REQUIRE(platform::file_exists(f.session_dir + "/snapshot/config.json"));

    write_file(f.graph, "digraph flow { changed; }\n");
    write_file(f.project + "/.needle/flow/stages/a/prompt.md", "prompt after\n");
    write_file(f.config, "{\"defaults\":{\"x\":\"after\"}}\n");

    auto restored = TroubleshootSnapshot::restore(f.project, f.session_dir);
    REQUIRE(restored.ok());
    REQUIRE(read_file(f.graph) == "digraph flow { a; }\n");
    REQUIRE(read_file(f.project + "/.needle/flow/stages/a/prompt.md") == "prompt before\n");
    REQUIRE(read_file(f.config) == "{\"defaults\":{\"x\":\"before\"}}\n");
}

TEST_CASE("TroubleshootSnapshot is no-op for off and diagnose",
          "[troubleshoot][snapshot]") {
    SnapshotFixture f;
    REQUIRE(TroubleshootSnapshot::capture(f.project, f.graph, f.session_dir,
                                          TroubleshootMode::Off).ok());
    REQUIRE_FALSE(platform::file_exists(f.session_dir + "/snapshot/manifest.json"));

    REQUIRE(TroubleshootSnapshot::capture(f.project, f.graph, f.session_dir,
                                          TroubleshootMode::Diagnose).ok());
    REQUIRE_FALSE(platform::file_exists(f.session_dir + "/snapshot/manifest.json"));
}
