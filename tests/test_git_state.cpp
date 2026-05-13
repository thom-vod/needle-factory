#include <catch2/catch.hpp>
#include "needle/util/git_state.h"
#include "needle/platform/platform.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace needle;

namespace {

// Build a fresh, throwaway git repo under <tmp>/needle_git_state_<pid>_<n>
// for tests that need real git behaviour. We ensure_default identity so
// `git commit` works in CI environments without a configured user.
struct GitFixture {
    std::string root;

    GitFixture() {
        static int counter = 0;
        root = platform::temp_dir() + "/needle_git_state_test_" +
               std::to_string(getpid()) + "_" + std::to_string(counter++);
        std::string mkdir = "rm -rf '" + root + "' && mkdir -p '" + root + "'";
        (void)std::system(mkdir.c_str());
        run("git init -q");
        run("git config user.email needle-test@example.com");
        run("git config user.name 'Needle Test'");
        run("git config commit.gpgsign false");
    }

    ~GitFixture() {
        std::string rm = "rm -rf '" + root + "'";
        (void)std::system(rm.c_str());
    }

    int run(const std::string& cmd) {
        std::string full = "cd '" + root + "' && " + cmd + " >/dev/null 2>&1";
        return std::system(full.c_str());
    }

    void write_file(const std::string& path, const std::string& content) {
        std::ofstream f(root + "/" + path);
        f << content;
    }
};

} // namespace

TEST_CASE("GitStateRecorder: capture on non-git dir returns invalid",
          "[git_state]") {
    std::string tmp = platform::temp_dir() + "/needle_git_state_nongit_" +
                      std::to_string(getpid());
    std::string mkdir = "rm -rf '" + tmp + "' && mkdir -p '" + tmp + "'";
    (void)std::system(mkdir.c_str());

    GitStateSnapshot s = GitStateRecorder::capture(tmp);
    REQUIRE_FALSE(s.valid);
    REQUIRE(s.head.empty());

    std::string rm = "rm -rf '" + tmp + "'";
    (void)std::system(rm.c_str());
}

TEST_CASE("GitStateRecorder: capture on fresh repo with one commit",
          "[git_state]") {
    GitFixture f;
    f.write_file("a.txt", "hello\n");
    REQUIRE(f.run("git add a.txt && git commit -m initial") == 0);

    GitStateSnapshot s = GitStateRecorder::capture(f.root);
    REQUIRE(s.valid);
    REQUIRE_FALSE(s.head.empty());
    REQUIRE(s.untracked.empty());
    REQUIRE(s.modified.empty());
}

TEST_CASE("GitStateRecorder: snapshot reports untracked files",
          "[git_state]") {
    GitFixture f;
    f.write_file("a.txt", "x\n");
    REQUIRE(f.run("git add a.txt && git commit -m initial") == 0);
    f.write_file("untracked.txt", "y\n");

    GitStateSnapshot s = GitStateRecorder::capture(f.root);
    REQUIRE(s.valid);
    REQUIRE(s.untracked.size() == 1);
    REQUIRE(s.untracked[0] == "untracked.txt");
}

TEST_CASE("GitStateRecorder: diff reports newly added commit",
          "[git_state]") {
    GitFixture f;
    f.write_file("a.txt", "x\n");
    REQUIRE(f.run("git add a.txt && git commit -m initial") == 0);

    GitStateSnapshot before = GitStateRecorder::capture(f.root);
    REQUIRE(before.valid);

    f.write_file("b.txt", "y\n");
    REQUIRE(f.run("git add b.txt && git commit -m 'add b'") == 0);

    GitStateSnapshot after = GitStateRecorder::capture(f.root);
    REQUIRE(after.valid);
    REQUIRE(before.head != after.head);

    GitStateDelta delta = GitStateRecorder::diff(before, after, f.root);
    REQUIRE(delta.commits_added.size() == 1);
    REQUIRE(delta.commits_added[0].subject == "add b");
    REQUIRE_FALSE(delta.commits_added[0].hash.empty());
}

TEST_CASE("GitStateRecorder: diff reports newly untracked files",
          "[git_state]") {
    GitFixture f;
    f.write_file("a.txt", "x\n");
    REQUIRE(f.run("git add a.txt && git commit -m initial") == 0);

    GitStateSnapshot before = GitStateRecorder::capture(f.root);
    f.write_file("new1.txt", "1\n");
    f.write_file("new2.txt", "2\n");
    GitStateSnapshot after = GitStateRecorder::capture(f.root);

    GitStateDelta delta = GitStateRecorder::diff(before, after, f.root);
    REQUIRE(delta.commits_added.empty());
    REQUIRE(delta.files_added_untracked.size() == 2);
}

TEST_CASE("GitStateRecorder: diff reports newly modified files",
          "[git_state]") {
    GitFixture f;
    f.write_file("a.txt", "x\n");
    REQUIRE(f.run("git add a.txt && git commit -m initial") == 0);

    GitStateSnapshot before = GitStateRecorder::capture(f.root);
    f.write_file("a.txt", "x changed\n");
    GitStateSnapshot after = GitStateRecorder::capture(f.root);

    GitStateDelta delta = GitStateRecorder::diff(before, after, f.root);
    REQUIRE(delta.files_modified_uncommitted.size() == 1);
    REQUIRE(delta.files_modified_uncommitted[0] == "a.txt");
}

TEST_CASE("GitStateRecorder: to_json round-trip preserves structure",
          "[git_state]") {
    GitStateDelta d;
    d.commits_added.push_back({"deadbeef", "subject one"});
    d.files_added_untracked = {"foo.txt", "bar.txt"};
    d.files_modified_uncommitted = {"baz.cpp"};

    nlohmann::json j = GitStateRecorder::to_json(d);
    REQUIRE(j["commits_added"].is_array());
    REQUIRE(j["commits_added"].size() == 1);
    REQUIRE(j["commits_added"][0]["hash"] == "deadbeef");
    REQUIRE(j["commits_added"][0]["subject"] == "subject one");
    REQUIRE(j["files_added_untracked"].size() == 2);
    REQUIRE(j["files_modified_uncommitted"].size() == 1);
}
