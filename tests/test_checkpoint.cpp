#include <catch2/catch.hpp>
#include "needle/engine/checkpoint_manager.h"
#include <cstdio>
#include <fstream>
#include "needle/platform/platform.h"

using namespace needle;

namespace {

Checkpoint make_test_checkpoint() {
    Checkpoint cp;
    cp.timestamp = "2026-03-12T10:00:00Z";
    cp.current_node = "implement";
    cp.completed_nodes = {"start", "design"};
    cp.retry_counters["design"] = 2;
    cp.context.set("project", "needle");
    cp.context.set("mode", "fast");
    cp.graph_file = "pipeline.dot";
    cp.graph_hash = "abc123";
    cp.branch_worktrees["branch_a"] = "/tmp/repo-wt-run-branch_a";
    return cp;
}

} // anonymous namespace

TEST_CASE("Checkpoint: JSON round-trip", "[checkpoint]") {
    Checkpoint original = make_test_checkpoint();
    nlohmann::json j = original.to_json();

    auto result = Checkpoint::from_json(j);
    REQUIRE(result.ok());

    const Checkpoint& loaded = result.value();
    REQUIRE(loaded.timestamp == original.timestamp);
    REQUIRE(loaded.current_node == original.current_node);
    REQUIRE(loaded.completed_nodes == original.completed_nodes);
    REQUIRE(loaded.retry_counters.at("design") == 2);
    REQUIRE(loaded.context.get("project") == "needle");
    REQUIRE(loaded.context.get("mode") == "fast");
    REQUIRE(loaded.graph_file == original.graph_file);
    REQUIRE(loaded.graph_hash == original.graph_hash);
    REQUIRE(loaded.branch_worktrees.at("branch_a") == "/tmp/repo-wt-run-branch_a");
}

TEST_CASE("Checkpoint: to_json produces expected fields", "[checkpoint]") {
    Checkpoint cp = make_test_checkpoint();
    nlohmann::json j = cp.to_json();

    REQUIRE(j.contains("timestamp"));
    REQUIRE(j.contains("current_node"));
    REQUIRE(j.contains("completed_nodes"));
    REQUIRE(j.contains("retry_counters"));
    REQUIRE(j.contains("context"));
    REQUIRE(j.contains("graph_file"));
    REQUIRE(j.contains("graph_hash"));
    REQUIRE(j["completed_nodes"].size() == 2);
}

TEST_CASE("Checkpoint: from_json with invalid JSON returns failure", "[checkpoint]") {
    nlohmann::json j;
    j["timestamp"] = "2026-03-12T10:00:00Z";
    // Missing required fields

    auto result = Checkpoint::from_json(j);
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("InMemoryCheckpointWriter: save and load", "[checkpoint]") {
    InMemoryCheckpointWriter writer;
    Checkpoint cp = make_test_checkpoint();

    auto save_result = writer.save(cp, "test/checkpoint.json");
    REQUIRE(save_result.ok());

    auto load_result = writer.load("test/checkpoint.json");
    REQUIRE(load_result.ok());
    REQUIRE(load_result.value().current_node == "implement");
    REQUIRE(load_result.value().context.get("project") == "needle");
}

TEST_CASE("InMemoryCheckpointWriter: load non-existent returns failure", "[checkpoint]") {
    InMemoryCheckpointWriter writer;

    auto result = writer.load("nonexistent.json");
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("InMemoryCheckpointWriter: overwrite existing", "[checkpoint]") {
    InMemoryCheckpointWriter writer;
    Checkpoint cp1 = make_test_checkpoint();
    writer.save(cp1, "path.json");

    Checkpoint cp2;
    cp2.timestamp = "2026-03-12T12:00:00Z";
    cp2.current_node = "review";
    cp2.context.set("status", "reviewing");
    cp2.graph_file = "pipeline.dot";
    cp2.graph_hash = "def456";
    writer.save(cp2, "path.json");

    auto result = writer.load("path.json");
    REQUIRE(result.ok());
    REQUIRE(result.value().current_node == "review");
}

TEST_CASE("JsonCheckpointWriter: save and load from file", "[checkpoint]") {
    JsonCheckpointWriter writer;
    Checkpoint cp = make_test_checkpoint();
    std::string path = platform::temp_dir() + "/needle_test_checkpoint.json";

    auto save_result = writer.save(cp, path);
    REQUIRE(save_result.ok());

    auto load_result = writer.load(path);
    REQUIRE(load_result.ok());
    REQUIRE(load_result.value().current_node == "implement");
    REQUIRE(load_result.value().completed_nodes.size() == 2);

    // Verify temp file was removed (atomic write)
    std::string tmp_path = path + ".tmp";
    std::ifstream tmp_check(tmp_path);
    REQUIRE_FALSE(tmp_check.is_open());

    // Clean up
    std::remove(path.c_str());
}

TEST_CASE("JsonCheckpointWriter: load non-existent file returns failure", "[checkpoint]") {
    JsonCheckpointWriter writer;
    auto result = writer.load(platform::temp_dir() + "/needle_nonexistent_checkpoint_xyz.json");
    REQUIRE_FALSE(result.ok());
}
