#include <catch2/catch.hpp>
#include "needle/troubleshoot/diagnose.h"

#include "needle/platform/platform.h"

#include <cstdlib>
#include <fstream>
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
        dir = platform::temp_dir() + "/needle_tshoot_diag_" +
              std::to_string(getpid()) + "_" + std::to_string(counter++);
        std::system(("rm -rf '" + dir + "'").c_str());
        std::system(("mkdir -p '" + dir + "/stages'").c_str());
    }

    ~RunDirFixture() {
        std::system(("rm -rf '" + dir + "'").c_str());
    }

    void write_checkpoint(const std::string& json) {
        std::ofstream f(dir + "/checkpoint.json");
        f << json;
    }

    void write_stage_status(const std::string& node_id, const std::string& json) {
        std::system(("mkdir -p '" + dir + "/stages/" + node_id + "'").c_str());
        std::ofstream f(dir + "/stages/" + node_id + "/status.json");
        f << json;
    }
};

const std::string kBaseCheckpoint =
    R"({"timestamp":"x","current_node":"p5_red","completed_nodes":["p1"],)"
    R"("retry_counters":{},"context":{"needle.last_outcome.status":"FAILURE"},)"
    R"("graph_file":"g.dot","graph_hash":"abc"})";

} // namespace

TEST_CASE("Diagnose::classify — idle stall with uncommitted work",
          "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_checkpoint(kBaseCheckpoint);
    f.write_stage_status("p5_red", R"({
        "node_id": "p5_red",
        "status": "FAILURE",
        "output": "proc idle for 300s",
        "timeout_kind": "idle",
        "git_state": {
            "commits_added": [],
            "files_added_untracked": ["src/foo.cpp"],
            "files_modified_uncommitted": []
        }
    })");

    DiagnosisSignals s = Diagnose::collect(f.dir);
    REQUIRE(Diagnose::classify(s) == FailureKind::IdleStallWorkOnDisk);
    REQUIRE(s.timeout_kind == "idle");
    REQUIRE(s.files_added_untracked.size() == 1);
}

TEST_CASE("Diagnose::classify — idle stall with committed work",
          "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_checkpoint(kBaseCheckpoint);
    f.write_stage_status("p5_red", R"({
        "node_id": "p5_red",
        "status": "FAILURE",
        "timeout_kind": "idle",
        "git_state": {
            "commits_added": [{"hash":"abc1234","subject":"add stuff"}],
            "files_added_untracked": [],
            "files_modified_uncommitted": []
        }
    })");

    DiagnosisSignals s = Diagnose::collect(f.dir);
    REQUIRE(Diagnose::classify(s) == FailureKind::IdleStallWorkCommitted);
    REQUIRE(s.commits_added.size() == 1);
    REQUIRE(s.commits_added[0].subject == "add stuff");
}

TEST_CASE("Diagnose::classify — idle stall, nothing to salvage",
          "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_checkpoint(kBaseCheckpoint);
    f.write_stage_status("p5_red", R"({
        "node_id": "p5_red",
        "status": "FAILURE",
        "timeout_kind": "idle",
        "git_state": {
            "commits_added": [],
            "files_added_untracked": [],
            "files_modified_uncommitted": []
        }
    })");

    DiagnosisSignals s = Diagnose::collect(f.dir);
    REQUIRE(Diagnose::classify(s) == FailureKind::IdleStallNoWorkSalvageable);
}

TEST_CASE("Diagnose::classify — wall-clock timeout with progress",
          "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_checkpoint(kBaseCheckpoint);
    f.write_stage_status("p5_red", R"({
        "node_id": "p5_red",
        "status": "FAILURE",
        "timeout_kind": "wall_clock",
        "git_state": {
            "commits_added": [{"hash":"abc","subject":"work"}],
            "files_added_untracked": [],
            "files_modified_uncommitted": []
        }
    })");

    DiagnosisSignals s = Diagnose::collect(f.dir);
    REQUIRE(Diagnose::classify(s) == FailureKind::WallClockWithProgress);
}

TEST_CASE("Diagnose::classify — self-exit error (no timeout)",
          "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_checkpoint(kBaseCheckpoint);
    f.write_stage_status("p5_red", R"({
        "node_id": "p5_red",
        "status": "FAILURE",
        "output": "error CA1065: get_X creates exception"
    })");

    DiagnosisSignals s = Diagnose::collect(f.dir);
    REQUIRE_FALSE(s.timed_out);
    REQUIRE(Diagnose::classify(s) == FailureKind::SelfExitError);
}

TEST_CASE("Diagnose::collect — handles missing run dir gracefully",
          "[troubleshoot][diagnose]") {
    DiagnosisSignals s = Diagnose::collect("/tmp/needle_does_not_exist_xyzzy");
    REQUIRE(s.failed_node.empty());
    REQUIRE(s.completed_nodes.empty());
}

TEST_CASE("Diagnose::render_markdown — produces expected sections",
          "[troubleshoot][diagnose]") {
    DiagnosisSignals s;
    s.failed_node = "fix_node";
    s.status_status = "FAILURE";
    s.timeout_kind = "idle";
    s.timed_out = true;
    s.files_added_untracked = {"src/a.cpp"};

    std::string md = Diagnose::render_markdown(s, FailureKind::IdleStallWorkOnDisk);
    REQUIRE(md.find("# Needle Troubleshoot") != std::string::npos);
    REQUIRE(md.find("## Diagnosis") != std::string::npos);
    REQUIRE(md.find("idle_stall_with_uncommitted_work") != std::string::npos);
    REQUIRE(md.find("src/a.cpp") != std::string::npos);
    REQUIRE(md.find("## Likely root cause") != std::string::npos);
    REQUIRE(md.find("## Proposed actions") != std::string::npos);
}
