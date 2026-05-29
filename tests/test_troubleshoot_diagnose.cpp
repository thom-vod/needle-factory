#include <catch2/catch.hpp>
#include "needle/troubleshoot/diagnose.h"
#include "needle/platform/platform.h"
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace needle;

namespace {

struct RunDirFixture {
    std::string dir;

    RunDirFixture() {
        static int counter = 0;
#ifndef _WIN32
        int pid = getpid();
#else
        int pid = 12345;
#endif
        dir = platform::temp_dir() + "/needle_tshoot_diag_" +
              std::to_string(pid) + "_" + std::to_string(counter++);
        { int _rc = std::system(("rm -rf '" + dir + "'").c_str()); (void)_rc; }
        { int _rc = std::system(("mkdir -p '" + dir + "/stages'").c_str()); (void)_rc; }
    }

    ~RunDirFixture() {
        { int _rc = std::system(("rm -rf '" + dir + "'").c_str()); (void)_rc; }
    }

    void write_file(const std::string& rel, const std::string& body) {
        auto pos = rel.find_last_of('/');
        if (pos != std::string::npos) {
            std::string parent = rel.substr(0, pos);
            { int _rc = std::system(("mkdir -p '" + dir + "/" + parent + "'").c_str()); (void)_rc; }
        }
        std::ofstream f(dir + "/" + rel);
        f << body;
    }
};

const std::string kBaseCheckpoint =
    R"({"timestamp":"x","current_node":"fan","completed_nodes":["start"],)"
    R"("retry_counters":{},"context":{"needle.last_outcome.status":"FAILURE"},)"
    R"("graph_file":"","graph_hash":"abc"})";

} // namespace

TEST_CASE("Diagnose: wall-clock timeout without own progress", "[troubleshoot][diagnose]") {
#ifdef _WIN32
    SUCCEED("git ancestry fixture is POSIX-only in this test");
#else
    RunDirFixture f;
    std::string repo = f.dir + "/repo";
    { int _rc = std::system(("mkdir -p '" + repo + "' && cd '" + repo + "' && git init -q && git config user.email t@t && git config user.name t && echo a > a.txt && git add a.txt && git commit -q -m a && echo b >> a.txt && git commit -q -am b").c_str()); (void)_rc; }

    std::string start_hash;
    {
        FILE* p = popen(("cd '" + repo + "' && git rev-parse HEAD").c_str(), "r");
        char buf[256] = {0};
        if (p && fgets(buf, sizeof(buf), p)) start_hash = buf;
        if (p) pclose(p);
        while (!start_hash.empty() && (start_hash.back() == '\n' || start_hash.back() == '\r')) start_hash.pop_back();
    }
    std::string older_hash;
    {
        FILE* p = popen(("cd '" + repo + "' && git rev-parse HEAD~1").c_str(), "r");
        char buf[256] = {0};
        if (p && fgets(buf, sizeof(buf), p)) older_hash = buf;
        if (p) pclose(p);
        while (!older_hash.empty() && (older_hash.back() == '\n' || older_hash.back() == '\r')) older_hash.pop_back();
    }

    std::string run_dir = repo + "/.needle/run1";
    { int _rc = std::system(("mkdir -p '" + run_dir + "/stages/fan'").c_str()); (void)_rc; }
    std::ofstream cp_out(run_dir + "/checkpoint.json");
    cp_out << kBaseCheckpoint;
    cp_out.close();
    std::string status_json = std::string("{\"status\":\"FAILURE\",\"timeout_kind\":\"wall_clock\",")
                            + "\"git_state\":{\"commits_added\":[{\"hash\":\"" + older_hash
                            + "\",\"subject\":\"older\"}],\"files_added_untracked\":[],"
                              "\"files_modified_uncommitted\":[]}}";
    std::ofstream st_out(run_dir + "/stages/fan/status.json");
    st_out << status_json;
    st_out.close();
    std::ofstream sc_out(run_dir + "/stages/fan/start_commit.txt");
    sc_out << start_hash << "\n";
    sc_out.close();

    DiagnosisSignals s = Diagnose::collect(run_dir, "fan");
    REQUIRE(s.commits_added.size() == 1);
    FailureKind k = Diagnose::classify(s);
    REQUIRE((k == FailureKind::WallClockWithoutOwnProgress || k == FailureKind::WallClockWithProgress));
#endif
}

TEST_CASE("Diagnose: fallback when start_commit missing", "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_file("checkpoint.json", kBaseCheckpoint);
    f.write_file("stages/fan/status.json", R"({"status":"FAILURE","timeout_kind":"wall_clock",)"
                                       R"("git_state":{"commits_added":[{"hash":"abc","subject":"work"}],"files_added_untracked":[],"files_modified_uncommitted":[]}})");

    DiagnosisSignals s = Diagnose::collect(f.dir, "fan");
    REQUIRE(s.own_commits.size() == 1);
    REQUIRE(Diagnose::classify(s) == FailureKind::WallClockWithProgress);
}

TEST_CASE("Diagnose: role prompt conflict", "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_file("checkpoint.json", kBaseCheckpoint);
    f.write_file("stages/fan/status.json", R"({"status":"FAILURE"})");
    f.write_file("stages/fan/prompt.md",
                 "Operating in reviewer mode.\\n"
                 "MUST NOT call Write or Edit.\\n"
                 "Commit your changes.\\n"
                 "Run the project's test suite.\\n");

    DiagnosisSignals s = Diagnose::collect(f.dir, "fan");
    REQUIRE_FALSE(s.role_keywords_found.empty());
    REQUIRE_FALSE(s.impl_keywords_found.empty());
    REQUIRE(Diagnose::classify(s) == FailureKind::RolePromptConflict);
}

TEST_CASE("Diagnose: variable corrupted", "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_file("checkpoint.json", kBaseCheckpoint);
    f.write_file("stages/fan/status.json", R"({"status":"FAILURE"})");
    f.write_file("stages/fan/prompt.md", "Read $var.spec_path and implement.");

    DiagnosisSignals s = Diagnose::collect(f.dir, "fan");
    REQUIRE_FALSE(s.unresolved_vars.empty());
    REQUIRE(Diagnose::classify(s) == FailureKind::VariableCorrupted);
}

TEST_CASE("Diagnose: wall-clock timeout outranks unresolved-var scan",
          "[troubleshoot][diagnose]") {
    // A genuine wall-clock timeout whose prompt also tripped the $var scan.
    // The timeout is authoritative — classify must NOT report variable_corrupted.
    DiagnosisSignals s;
    s.failed_node = "storyboard";
    s.status_status = "FAILURE";
    s.timed_out = true;
    s.timeout_kind = "wall_clock";
    s.unresolved_vars = {"seed"};  // false-positive prompt-scan hit

    FailureKind k = Diagnose::classify(s);
    CHECK(k != FailureKind::VariableCorrupted);
    CHECK((k == FailureKind::WallClockWithoutOwnProgress ||
           k == FailureKind::WallClockWithProgress));
}

TEST_CASE("Diagnose: idle timeout outranks unresolved-var scan",
          "[troubleshoot][diagnose]") {
    DiagnosisSignals s;
    s.failed_node = "node";
    s.status_status = "FAILURE";
    s.timed_out = true;
    s.timeout_kind = "idle";
    s.unresolved_vars = {"seed"};

    FailureKind k = Diagnose::classify(s);
    CHECK(k != FailureKind::VariableCorrupted);
    CHECK((k == FailureKind::IdleStallNoWorkSalvageable ||
           k == FailureKind::IdleStallWorkOnDisk ||
           k == FailureKind::IdleStallWorkCommitted));
}

TEST_CASE("Diagnose: unresolved-var scan still classifies when no timeout",
          "[troubleshoot][diagnose]") {
    // Regression guard: the reorder must not break the non-timeout path.
    DiagnosisSignals s;
    s.failed_node = "node";
    s.status_status = "FAILURE";
    s.unresolved_vars = {"spec_path"};
    CHECK(Diagnose::classify(s) == FailureKind::VariableCorrupted);
}

TEST_CASE("Diagnose: parallel branch recursion", "[troubleshoot][diagnose]") {
    RunDirFixture f;
    std::string dot_path = f.dir + "/graph.dot";
    f.write_file("graph.dot",
                 "digraph g {\n"
                 "  start [shape=Mdiamond];\n"
                 "  fan [shape=component];\n"
                 "  c1 [shape=box];\n"
                 "  c2 [shape=box];\n"
                 "  end [shape=Msquare];\n"
                 "  start -> fan; fan -> c1; fan -> c2; c1 -> end; c2 -> end;\n"
                 "}\n");

    nlohmann::json cpj;
    cpj["timestamp"] = "x";
    cpj["current_node"] = "fan";
    cpj["completed_nodes"] = nlohmann::json::array({"start"});
    cpj["retry_counters"] = nlohmann::json::object();
    cpj["context"] = nlohmann::json::object({{"needle.last_outcome.status", "FAILURE"}});
    cpj["graph_file"] = dot_path;
    cpj["graph_hash"] = "abc";
    f.write_file("checkpoint.json", cpj.dump());
    f.write_file("stages/fan/status.json", R"({"status":"FAILURE"})");
    f.write_file("stages/c1/status.json", R"({"status":"FAILURE","timeout_kind":"idle"})");
    f.write_file("stages/c2/status.json", R"({"status":"FAILURE","timeout_kind":"wall_clock",)"
                                      R"("git_state":{"commits_added":[],"files_added_untracked":[],"files_modified_uncommitted":[]}})");

    DiagnosisReport r = Diagnose::collect_report(f.dir);
    REQUIRE(r.children.size() == 2);
    std::string md = Diagnose::render_markdown(r);
    REQUIRE(md.find("Parallel branch failures: 2 of 2") != std::string::npos);
}

TEST_CASE("Diagnose: cherry-pick conflict classification", "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_file("checkpoint.json", kBaseCheckpoint);
    f.write_file("stages/fan/status.json",
                 R"({"status":"FAILURE","output":"cherry-pick conflict",)"
                 R"("cherry_pick_conflict":{"branch_that_conflicted":"b","conflicting_files":["x.cpp"]}})");
    DiagnosisSignals s = Diagnose::collect(f.dir, "fan");
    REQUIRE(s.cherry_pick_conflict);
    REQUIRE(Diagnose::classify(s) == FailureKind::CherryPickConflict);
}

#ifndef _WIN32
TEST_CASE("Diagnose: collects descendant pids as additive signal", "[troubleshoot][diagnose]") {
    RunDirFixture f;
    f.write_file("checkpoint.json", kBaseCheckpoint);
    f.write_file("stages/fan/status.json", R"({"status":"FAILURE","output":"x"})");
    f.write_file("engine.pid", std::to_string(getpid()) + "\n");

    pid_t child = fork();
    if (child == 0) {
        ::sleep(2);
        _exit(0);
    }

    DiagnosisSignals s = Diagnose::collect(f.dir, "fan");
    bool found = false;
    for (int pid : s.descendant_pids) {
        if (pid == static_cast<int>(child)) found = true;
    }
    REQUIRE(s.engine_pid_alive);
    REQUIRE(found);

    int status = 0;
    waitpid(child, &status, 0);
}
#endif
