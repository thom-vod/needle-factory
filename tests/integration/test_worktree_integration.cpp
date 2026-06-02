#include <catch2/catch.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "needle/engine/fan_in_merger.h"
#include "needle/backend/process_runner.h"
#include "needle/platform/platform.h"

using namespace needle;

namespace {

// Run git via the process runner (no shell), so the fixture works on Windows
// as well as POSIX. (The fan-in code under test now does the same.)
void git_ok(const std::string& dir, const std::vector<std::string>& args) {
    NativeProcessRunner runner;
    auto r = runner.run("git", args, dir, 20000);
    int code = r.ok() ? r.value().exit_code : -1;
    if (code != 0) {
        std::string joined;
        for (const auto& a : args) joined += a + " ";
        UNSCOPED_INFO("git " << joined << "(cwd=" << dir << ") exit=" << code
                      << " out=[" << (r.ok() ? r.value().stdout_output : std::string("<launch failed>"))
                      << "] err=[" << (r.ok() ? r.value().stderr_output : std::string()) << "]");
    }
    REQUIRE(code == 0);
}

std::string git_capture(const std::string& dir, const std::vector<std::string>& args) {
    NativeProcessRunner runner;
    auto r = runner.run("git", args, dir, 20000);
    if (!r.ok()) return "";
    std::string out = r.value().stdout_output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

void write_file(const std::string& path, const std::string& content, bool append = false) {
    std::ofstream f(path, append ? std::ios::app : std::ios::trunc);
    f << content;
}

// Cross-platform replacement for tests/data/worktree_fixtures/init_repo.sh:
// a baseline repo at <root>/repo with one commit, returns the repo path.
std::string init_repo(const std::string& root) {
    std::string repo = root + "/repo";
    platform::remove_recursive(repo);
    platform::mkdir_p(repo);
    git_ok(repo, {"init", "-q"});
    git_ok(repo, {"config", "user.email", "test@example.com"});
    git_ok(repo, {"config", "user.name", "needle-tests"});
    git_ok(repo, {"config", "commit.gpgsign", "false"});
    write_file(repo + "/shared.txt", "base\n");
    git_ok(repo, {"add", "shared.txt"});
    git_ok(repo, {"commit", "-m", "baseline", "-q"});
    return repo;
}

} // namespace

TEST_CASE("Worktree integration: clean fan-in cherry-picks branch commits",
          "[integration][worktree]") {
    std::string root = platform::temp_dir() + "/needle_wt_it_clean";
    platform::remove_recursive(root);
    std::string launch = init_repo(root);
    std::string launch_commit = git_capture(launch, {"rev-parse", "HEAD"});

    // branch a: add a new file
    git_ok(launch, {"worktree", "add", root + "/repo-wt-a", "-b", "auto/run/a"});
    write_file(root + "/repo-wt-a/file_a.txt", "a\n", /*append=*/true);
    git_ok(root + "/repo-wt-a", {"add", "."});
    git_ok(root + "/repo-wt-a", {"commit", "-m", "a", "-q"});
    // branch b: add a different new file
    git_ok(launch, {"worktree", "add", root + "/repo-wt-b", "-b", "auto/run/b"});
    write_file(root + "/repo-wt-b/file_b.txt", "b\n", /*append=*/true);
    git_ok(root + "/repo-wt-b", {"add", "."});
    git_ok(root + "/repo-wt-b", {"commit", "-m", "b", "-q"});

    Context ctx;
    ctx.set("needle.branch_worktree.a", root + "/repo-wt-a");
    ctx.set("needle.branch_worktree.b", root + "/repo-wt-b");
    WorktreeConfig cfg;
    cfg.cleanup = "keep";
    auto merged = FanInMerger::merge(launch, launch_commit, {"b", "a"}, ctx, cfg);
    REQUIRE(merged.ok());
    REQUIRE(merged.value().ok);

    std::string history = git_capture(launch, {"log", "--oneline", "-n", "3"});
    REQUIRE(history.find(" a") != std::string::npos);
    REQUIRE(history.find(" b") != std::string::npos);

    platform::remove_recursive(root);
}

TEST_CASE("Worktree integration: conflict produces structured conflict payload",
          "[integration][worktree]") {
    std::string root = platform::temp_dir() + "/needle_wt_it_conflict";
    platform::remove_recursive(root);
    std::string launch = init_repo(root);
    std::string launch_commit = git_capture(launch, {"rev-parse", "HEAD"});

    // Both branches overwrite the same file -> cherry-pick conflict.
    git_ok(launch, {"worktree", "add", root + "/repo-wt-a", "-b", "auto/run/a"});
    write_file(root + "/repo-wt-a/shared.txt", "conflict-a\n");
    git_ok(root + "/repo-wt-a", {"add", "shared.txt"});
    git_ok(root + "/repo-wt-a", {"commit", "-m", "a", "-q"});
    git_ok(launch, {"worktree", "add", root + "/repo-wt-b", "-b", "auto/run/b"});
    write_file(root + "/repo-wt-b/shared.txt", "conflict-b\n");
    git_ok(root + "/repo-wt-b", {"add", "shared.txt"});
    git_ok(root + "/repo-wt-b", {"commit", "-m", "b", "-q"});

    Context ctx;
    ctx.set("needle.branch_worktree.a", root + "/repo-wt-a");
    ctx.set("needle.branch_worktree.b", root + "/repo-wt-b");
    WorktreeConfig cfg;
    auto merged = FanInMerger::merge(launch, launch_commit, {"a", "b"}, ctx, cfg);
    REQUIRE(merged.ok());
    REQUIRE_FALSE(merged.value().ok);
    REQUIRE(merged.value().conflict.branch_that_conflicted == "b");
    REQUIRE_FALSE(merged.value().conflict.conflicting_files.empty());

    platform::remove_recursive(root);
}
