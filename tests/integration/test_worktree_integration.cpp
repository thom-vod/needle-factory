#include <catch2/catch.hpp>

#ifndef _WIN32

#include <cstdlib>
#include <fstream>

#include "needle/engine/fan_in_merger.h"

using namespace needle;

namespace {

std::string run(const std::string& cmd) {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    std::string out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    pclose(fp);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

void must(int rc) { REQUIRE(rc == 0); }

} // namespace

TEST_CASE("Worktree integration: clean fan-in cherry-picks branch commits",
          "[integration][worktree]") {
    std::string root = "/tmp/needle_wt_it_clean";
    must(std::system(("rm -rf " + root).c_str()));
    must(std::system(("mkdir -p " + root).c_str()));
    must(std::system(("sh tests/data/worktree_fixtures/init_repo.sh " + root).c_str()));

    std::string launch = root + "/repo";
    std::string launch_commit = run("git -C " + launch + " rev-parse HEAD");

    // branch a
    must(std::system(("git -C " + launch + " worktree add " + root + "/repo-wt-a -b auto/run/a").c_str()));
    must(std::system(("sh -lc 'echo a >> " + root + "/repo-wt-a/file_a.txt && git -C " + root + "/repo-wt-a add . && git -C " + root + "/repo-wt-a commit -m a -q'").c_str()));
    // branch b
    must(std::system(("git -C " + launch + " worktree add " + root + "/repo-wt-b -b auto/run/b").c_str()));
    must(std::system(("sh -lc 'echo b >> " + root + "/repo-wt-b/file_b.txt && git -C " + root + "/repo-wt-b add . && git -C " + root + "/repo-wt-b commit -m b -q'").c_str()));

    Context ctx;
    ctx.set("needle.branch_worktree.a", root + "/repo-wt-a");
    ctx.set("needle.branch_worktree.b", root + "/repo-wt-b");
    WorktreeConfig cfg;
    cfg.cleanup = "keep";
    auto merged = FanInMerger::merge(launch, launch_commit, {"b", "a"}, ctx, cfg);
    REQUIRE(merged.ok());
    REQUIRE(merged.value().ok);

    std::string history = run("git -C " + launch + " log --oneline -n 3");
    REQUIRE(history.find(" a") != std::string::npos);
    REQUIRE(history.find(" b") != std::string::npos);
}

TEST_CASE("Worktree integration: conflict produces structured conflict payload",
          "[integration][worktree]") {
    std::string root = "/tmp/needle_wt_it_conflict";
    must(std::system(("rm -rf " + root).c_str()));
    must(std::system(("mkdir -p " + root).c_str()));
    must(std::system(("sh tests/data/worktree_fixtures/init_repo.sh " + root).c_str()));
    std::string launch = root + "/repo";
    std::string launch_commit = run("git -C " + launch + " rev-parse HEAD");

    must(std::system(("git -C " + launch + " worktree add " + root + "/repo-wt-a -b auto/run/a").c_str()));
    must(std::system(("sh -lc 'echo conflict-a > " + root + "/repo-wt-a/shared.txt && git -C " + root + "/repo-wt-a add shared.txt && git -C " + root + "/repo-wt-a commit -m a -q'").c_str()));
    must(std::system(("git -C " + launch + " worktree add " + root + "/repo-wt-b -b auto/run/b").c_str()));
    must(std::system(("sh -lc 'echo conflict-b > " + root + "/repo-wt-b/shared.txt && git -C " + root + "/repo-wt-b add shared.txt && git -C " + root + "/repo-wt-b commit -m b -q'").c_str()));

    Context ctx;
    ctx.set("needle.branch_worktree.a", root + "/repo-wt-a");
    ctx.set("needle.branch_worktree.b", root + "/repo-wt-b");
    WorktreeConfig cfg;
    auto merged = FanInMerger::merge(launch, launch_commit, {"a", "b"}, ctx, cfg);
    REQUIRE(merged.ok());
    REQUIRE_FALSE(merged.value().ok);
    REQUIRE(merged.value().conflict.branch_that_conflicted == "b");
    REQUIRE_FALSE(merged.value().conflict.conflicting_files.empty());
}

#endif
