#include <catch2/catch.hpp>
#include "needle/worktree/manager.h"
#include "needle/platform/platform.h"

#include <cstdlib>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace needle;

namespace {

// Throwaway parent git repo + a sibling slot for a worktree.
struct WorktreeFixture {
    std::string parent_repo;
    std::string worktree_path;

    WorktreeFixture() {
        static int counter = 0;
        std::string root = platform::temp_dir() + "/needle_wt_test_" +
                           std::to_string(getpid()) + "_" + std::to_string(counter++);
        parent_repo = root + "/parent";
        worktree_path = root + "/wt";
        (void)std::system(("rm -rf '" + root + "' && mkdir -p '" + parent_repo + "'").c_str());
        run_in_parent("git init -q");
        run_in_parent("git config user.email needle-test@example.com");
        run_in_parent("git config user.name 'Needle Test'");
        run_in_parent("git config commit.gpgsign false");
        // Need at least one commit so worktree creation works.
        std::ofstream f(parent_repo + "/README");
        f << "x\n";
        f.close();
        run_in_parent("git add README && git commit -m initial");
    }

    ~WorktreeFixture() {
        std::string root = parent_repo.substr(0, parent_repo.size() - 7);
        (void)std::system(("rm -rf '" + root + "'").c_str());
    }

    int run_in_parent(const std::string& cmd) {
        return std::system(("cd '" + parent_repo + "' && " + cmd + " >/dev/null 2>&1").c_str());
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
    (void)std::system(("rm -rf '" + tmp + "' && mkdir -p '" + tmp + "'").c_str());

    WorktreeConfig cfg;
    cfg.strategy = WorktreeStrategy::Auto;
    cfg.path = tmp + "/wt";
    cfg.branch = "main";

    auto r = WorktreeManager::ensure_ready(tmp, cfg);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().find("not a git repo") != std::string::npos);

    (void)std::system(("rm -rf '" + tmp + "'").c_str());
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
