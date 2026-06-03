#include <catch2/catch.hpp>
#include "needle/worktree/manager.h"
#include "needle/platform/platform.h"
#include "needle/backend/process_runner.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace needle;

namespace {

// Throwaway parent git repo + a sibling slot for a worktree.
struct WorktreeFixture {
    std::string root;
    std::string parent_repo;
    std::string worktree_path;

    WorktreeFixture() {
        static int counter = 0;
        root = platform::temp_dir() + "/needle_wt_test_" +
               std::to_string(getpid()) + "_" + std::to_string(counter++);
        parent_repo = root + "/parent";
        worktree_path = root + "/wt";
        platform::remove_recursive(root);
        platform::mkdir_p(parent_repo);
        git_in_parent({"init", "-q"});
        git_in_parent({"config", "user.email", "needle-test@example.com"});
        git_in_parent({"config", "user.name", "Needle Test"});
        git_in_parent({"config", "commit.gpgsign", "false"});
        // Need at least one commit so worktree creation works.
        std::ofstream f(parent_repo + "/README");
        f << "x\n";
        f.close();
        git_in_parent({"add", "README"});
        git_in_parent({"commit", "-m", "initial"});
    }

    ~WorktreeFixture() {
        platform::remove_recursive(root);
    }

    // Run git in the parent repo via NativeProcessRunner (no shell), so it
    // works identically on POSIX and Windows; the old `cd '...' && git ...`
    // string went through cmd.exe on Windows and failed. Returns the exit
    // code, or -1 if git could not be launched.
    int git_in_parent(const std::vector<std::string>& args) {
        NativeProcessRunner runner;
        auto r = runner.run("git", args, parent_repo, 10000);
        return r.ok() ? r.value().exit_code : -1;
    }
};

} // namespace

TEST_CASE("WorktreeManager::ensure_ready creates a worktree on a fresh path",
          "[worktree][manager]") {
    WorktreeFixture f;
    WorktreeConfig cfg;
    cfg.strategy = WorktreeStrategy::Auto;
    cfg.path = f.worktree_path;
    cfg.branch = "feature/test-1";

    auto r = WorktreeManager::ensure_ready(f.parent_repo, cfg);
    REQUIRE(r.ok());
    REQUIRE(r.value().created_now);
    REQUIRE(r.value().branch == "feature/test-1");

    // Verify the worktree dir exists and is on the right branch.
    REQUIRE(WorktreeManager::is_active_worktree(cfg.path));
}

TEST_CASE("WorktreeManager::ensure_ready is idempotent on existing path/branch",
          "[worktree][manager]") {
    WorktreeFixture f;
    WorktreeConfig cfg;
    cfg.strategy = WorktreeStrategy::Auto;
    cfg.path = f.worktree_path;
    cfg.branch = "feature/test-2";

    auto r1 = WorktreeManager::ensure_ready(f.parent_repo, cfg);
    REQUIRE(r1.ok());
    REQUIRE(r1.value().created_now);

    auto r2 = WorktreeManager::ensure_ready(f.parent_repo, cfg);
    REQUIRE(r2.ok());
    REQUIRE_FALSE(r2.value().created_now);
}

TEST_CASE("WorktreeManager::ensure_ready fails when worktree on wrong branch",
          "[worktree][manager]") {
    WorktreeFixture f;
    WorktreeConfig cfg;
    cfg.strategy = WorktreeStrategy::Auto;
    cfg.path = f.worktree_path;
    cfg.branch = "feature/test-3";
    REQUIRE(WorktreeManager::ensure_ready(f.parent_repo, cfg).ok());

    // Now ask for the same path on a different branch — must fail.
    cfg.branch = "feature/test-3-different";
    auto r = WorktreeManager::ensure_ready(f.parent_repo, cfg);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().find("branch") != std::string::npos);
}

TEST_CASE("WorktreeManager::ensure_ready fails on non-git directory",
          "[worktree][manager]") {
    std::string tmp = platform::temp_dir() + "/needle_wt_nongit_" +
                      std::to_string(getpid());
    platform::remove_recursive(tmp);
    platform::mkdir_p(tmp);

    WorktreeConfig cfg;
    cfg.strategy = WorktreeStrategy::Auto;
    cfg.path = tmp + "/wt";
    cfg.branch = "main";

    auto r = WorktreeManager::ensure_ready(tmp, cfg);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().find("not a git repo") != std::string::npos);

    platform::remove_recursive(tmp);
}

TEST_CASE("WorktreeManager::ensure_ready fails when path missing or branch missing",
          "[worktree][manager]") {
    WorktreeFixture f;

    WorktreeConfig cfg;
    cfg.strategy = WorktreeStrategy::Auto;
    cfg.path = "";
    cfg.branch = "feature/test-x";
    REQUIRE_FALSE(WorktreeManager::ensure_ready(f.parent_repo, cfg).ok());

    cfg.path = f.worktree_path;
    cfg.branch = "";
    REQUIRE_FALSE(WorktreeManager::ensure_ready(f.parent_repo, cfg).ok());
}
