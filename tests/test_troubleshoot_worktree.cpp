#include <catch2/catch.hpp>

#include "needle/platform/platform.h"
#include "needle/worktree/strategy.h"

#include <cstdlib>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace needle;

namespace {

#ifndef _WIN32
struct GitFixture {
    std::string root;
    std::string project;
    std::string run_dir;
    std::string session_dir;

    GitFixture(const std::string& suffix) {
        root = platform::temp_dir() + "/needle_ts_wt_" + suffix + "_" + std::to_string(getpid());
        platform::remove_recursive(root);
        project = root + "/project";
        run_dir = project + "/.needle/flow";
        session_dir = run_dir + "/troubleshoot/session-test";
        platform::mkdir_p(project);
        platform::mkdir_p(session_dir);
        run("git init -q");
        run("git config user.email needle-test@example.com");
        run("git config user.name 'Needle Test'");
        run("git config commit.gpgsign false");
        write(project + "/README.md", "base\n");
        run("git add README.md && git commit -qm initial");
    }

    ~GitFixture() {
        platform::remove_recursive(root);
    }

    void write(const std::string& path, const std::string& value) {
        size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) platform::mkdir_p(path.substr(0, slash));
        std::ofstream out(path);
        out << value;
    }

    int run(const std::string& cmd) {
        return std::system(("cd '" + project + "' && " + cmd + " >/dev/null 2>&1").c_str());
    }
};
#endif

} // namespace

TEST_CASE("TroubleshootWorktree creates and applies fast-forward branch",
          "[troubleshoot][worktree]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f("apply");
    auto created = TroubleshootWorktree::create(f.project, "run-apply", f.session_dir);
    REQUIRE(created.ok());
    REQUIRE(platform::is_directory(created.value()));
    REQUIRE(platform::file_exists(f.session_dir + "/worktree/branch.txt"));

    std::ofstream out(created.value() + "/feature.txt");
    out << "from worktree\n";
    out.close();
    REQUIRE(std::system(("cd '" + created.value() + "' && git add feature.txt && git commit -qm feature").c_str()) == 0);

    auto applied = TroubleshootWorktree::apply(f.project, "run-apply");
    REQUIRE(applied.ok());
    REQUIRE(platform::file_exists(f.project + "/feature.txt"));
    REQUIRE_FALSE(platform::is_directory(created.value()));
#endif
}

TEST_CASE("TroubleshootWorktree discards worktree branch",
          "[troubleshoot][worktree]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f("discard");
    auto created = TroubleshootWorktree::create(f.project, "run-discard", f.session_dir);
    REQUIRE(created.ok());
    REQUIRE(platform::is_directory(created.value()));

    auto discarded = TroubleshootWorktree::discard(f.project, "run-discard");
    REQUIRE(discarded.ok());
    REQUIRE_FALSE(platform::is_directory(created.value()));
    REQUIRE(f.run("git rev-parse --verify auto/troubleshoot/run-discard") != 0);
#endif
}
