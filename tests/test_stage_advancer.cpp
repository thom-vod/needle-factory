#include <catch2/catch.hpp>
#include "needle/engine/stage_advancer.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/platform/platform.h"
#include "needle/util/fs_helpers.h"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace needle;

namespace {

struct RunDirFixture {
    std::string dir;

    RunDirFixture() {
        static int counter = 0;
        dir = platform::temp_dir() + "/needle_stage_advancer_" +
              std::to_string(getpid()) + "_" + std::to_string(counter++);
        // Use platform helpers rather than `rm -rf`/`mkdir -p` via std::system,
        // which fail on Windows (cmd.exe has neither). mkdir_p creates parents,
        // so this also makes the run dir itself.
        platform::remove_recursive(dir);
        platform::mkdir_p(dir + "/stages");
    }

    ~RunDirFixture() {
        platform::remove_recursive(dir);
    }

    void seed_checkpoint(const Checkpoint& cp) {
        JsonCheckpointWriter w;
        REQUIRE(w.save(cp, dir + "/checkpoint.json").ok());
    }

    Checkpoint load_checkpoint() const {
        JsonCheckpointWriter w;
        auto r = w.load(dir + "/checkpoint.json");
        REQUIRE(r.ok());
        return r.value();
    }
};

Checkpoint make_seed_cp() {
    Checkpoint cp;
    cp.timestamp = "2026-01-01T00:00:00Z";
    cp.current_node = "p5_red";
    cp.completed_nodes = {"p1", "p2", "p3", "p4_green"};
    cp.context.set("needle.last_outcome.status", "FAILURE");
    cp.graph_file = "graph.dot";
    cp.graph_hash = "abc123";
    return cp;
}

} // namespace

TEST_CASE("StageAdvancer::mark adds node to completed_nodes on success",
          "[stage_advancer]") {
    RunDirFixture f;
    f.seed_checkpoint(make_seed_cp());

    auto result = StageAdvancer::mark(f.dir, "p5_red", true,
                                      "manual recovery — fixed config");
    REQUIRE(result.ok());

    Checkpoint cp = f.load_checkpoint();
    REQUIRE(std::find(cp.completed_nodes.begin(), cp.completed_nodes.end(), "p5_red")
            != cp.completed_nodes.end());
    REQUIRE(cp.context.get("needle.last_outcome.status") == "SUCCESS");
    REQUIRE(cp.context.get("codergen.p5_red.output") ==
            "manual recovery — fixed config");
    REQUIRE(cp.current_node == "p5_red");
}

TEST_CASE("StageAdvancer::mark removes node from completed_nodes on failure",
          "[stage_advancer]") {
    RunDirFixture f;
    Checkpoint cp = make_seed_cp();
    cp.completed_nodes.push_back("p5_red");
    f.seed_checkpoint(cp);

    auto result = StageAdvancer::mark(f.dir, "p5_red", false, "");
    REQUIRE(result.ok());

    Checkpoint loaded = f.load_checkpoint();
    REQUIRE(std::find(loaded.completed_nodes.begin(), loaded.completed_nodes.end(), "p5_red")
            == loaded.completed_nodes.end());
    REQUIRE(loaded.context.get("needle.last_outcome.status") == "FAILURE");
}

TEST_CASE("StageAdvancer::mark is idempotent under repeated success",
          "[stage_advancer]") {
    RunDirFixture f;
    f.seed_checkpoint(make_seed_cp());

    REQUIRE(StageAdvancer::mark(f.dir, "p5_red", true, "x").ok());
    REQUIRE(StageAdvancer::mark(f.dir, "p5_red", true, "x").ok());

    Checkpoint cp = f.load_checkpoint();
    int count = 0;
    for (const auto& n : cp.completed_nodes) {
        if (n == "p5_red") ++count;
    }
    REQUIRE(count == 1);
}

TEST_CASE("StageAdvancer::mark writes a stage status.json",
          "[stage_advancer]") {
    RunDirFixture f;
    f.seed_checkpoint(make_seed_cp());

    REQUIRE(StageAdvancer::mark(f.dir, "p5_red", true, "summary text").ok());

    std::string status_path = f.dir + "/stages/p5_red/status.json";
    REQUIRE(platform::file_exists(status_path));

    std::ifstream in(status_path);
    nlohmann::json status;
    in >> status;
    REQUIRE(status["node_id"] == "p5_red");
    REQUIRE(status["status"] == "SUCCESS");
    REQUIRE(status["output"] == "summary text");
}

TEST_CASE("StageAdvancer::advance sets current_node",
          "[stage_advancer]") {
    RunDirFixture f;
    f.seed_checkpoint(make_seed_cp());

    REQUIRE(StageAdvancer::advance(f.dir, "p6_verify").ok());
    Checkpoint cp = f.load_checkpoint();
    REQUIRE(cp.current_node == "p6_verify");
}

TEST_CASE("StageAdvancer::advance with empty target returns failure",
          "[stage_advancer]") {
    RunDirFixture f;
    f.seed_checkpoint(make_seed_cp());

    auto result = StageAdvancer::advance(f.dir, "");
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("StageAdvancer::mark fails on missing checkpoint",
          "[stage_advancer]") {
    RunDirFixture f;
    // No checkpoint seeded.
    auto result = StageAdvancer::mark(f.dir, "p5_red", true, "x");
    REQUIRE_FALSE(result.ok());
}
