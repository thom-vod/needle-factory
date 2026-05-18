// SPRINT-016 Phase 2: backup-branch capture/rollback round-trip against
// a real `git init` fixture. The test is POSIX-only — git fixtures on
// Windows shell escapes are more trouble than the coverage is worth here
// for this sprint.

#include <catch2/catch.hpp>

#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "needle/engine/troubleshoot_backup.h"
#include "needle/platform/platform.h"

using namespace needle;

namespace {

struct GitFixture {
    std::string root;
    std::string project;
    std::string session_dir;

    GitFixture() {
        root = platform::temp_dir() + "/needle_backup_test_" +
               std::to_string(getpid()) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this));
        platform::remove_recursive(root);
        project = root + "/project";
        session_dir = root + "/run/troubleshoot/session-test";
        platform::mkdir_p(project);
        platform::mkdir_p(session_dir);
        // Seed a tracked file and a pre-existing untracked file.
        write_file(project + "/tracked.txt", "tracked-original\n");
        write_file(project + "/pre-existing-untracked.txt", "operator's file\n");
        const std::string script =
            "cd '" + project + "' && "
            "git init -q && "
            "git config user.email test@example.com && "
            "git config user.name 'Test User' && "
            "git config commit.gpgsign false && "
            "git add tracked.txt && "
            "git commit -qm 'initial'";
        REQUIRE(std::system(script.c_str()) == 0);
    }
    ~GitFixture() {
        platform::remove_recursive(root);
    }

    static void write_file(const std::string& path, const std::string& content) {
        std::ofstream out(path);
        REQUIRE(out.is_open());
        out << content;
    }

    static std::string read_file(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) return "";
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

} // namespace

TEST_CASE("Backup capture creates ref + base record + pre-untracked list", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-1", "test", f.session_dir);
    REQUIRE(captured.ok());
    REQUIRE(captured.value().branch == "auto/troubleshoot/backup/run-1-test");
    REQUIRE(!captured.value().base_sha.empty());
    REQUIRE(platform::file_exists(f.session_dir + "/backup-base.txt"));
    REQUIRE(platform::file_exists(f.session_dir + "/pre-untracked.txt"));
    REQUIRE(platform::file_exists(f.session_dir + "/backup-branch.txt"));
    REQUIRE(platform::file_exists(f.session_dir + "/current-branch.txt"));
    REQUIRE(platform::file_exists(f.session_dir + "/pre-modified.txt"));
    REQUIRE(!captured.value().current_branch.empty());
    REQUIRE(captured.value().pre_modified.empty());

    // pre-untracked.txt should contain the operator's untracked file.
    auto pre = f.read_file(f.session_dir + "/pre-untracked.txt");
    REQUIRE(pre.find("pre-existing-untracked.txt") != std::string::npos);

    // Branch ref should exist in git.
    REQUIRE(std::system(("cd '" + f.project + "' && git rev-parse --verify -q auto/troubleshoot/backup/run-1-test >/dev/null").c_str()) == 0);
#endif
}

TEST_CASE("Backup rollback restores tracked files and reports untracked drift", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-2", "test", f.session_dir);
    REQUIRE(captured.ok());

    // Simulate agent edits: modify the tracked file and create a new file.
    GitFixture::write_file(f.project + "/tracked.txt", "tracked-MODIFIED-by-agent\n");
    REQUIRE(std::system(("cd '" + f.project + "' && git add tracked.txt && git commit -qm 'agent edit'").c_str()) == 0);
    GitFixture::write_file(f.project + "/agent-created.txt", "added by agent\n");

    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE(report.ok());

    // Tracked file should be back to original.
    REQUIRE(f.read_file(f.project + "/tracked.txt") == "tracked-original\n");

    // Pre-existing untracked file should still be there (rollback must not
    // touch operator's untracked files).
    REQUIRE(platform::file_exists(f.project + "/pre-existing-untracked.txt"));

    // Agent-created file should be reported as drift but not deleted.
    REQUIRE(platform::file_exists(f.project + "/agent-created.txt"));
    const auto& drift = report.value().untracked_drift;
    bool found_agent_file = false;
    bool found_pre_untracked = false;
    for (const auto& f_path : drift) {
        if (f_path == "agent-created.txt") found_agent_file = true;
        if (f_path == "pre-existing-untracked.txt") found_pre_untracked = true;
    }
    REQUIRE(found_agent_file);
    REQUIRE_FALSE(found_pre_untracked);

    // Branch should be deleted by rollback.
    REQUIRE(std::system(("cd '" + f.project + "' && git rev-parse --verify -q auto/troubleshoot/backup/run-2-test >/dev/null").c_str()) != 0);
#endif
}

TEST_CASE("Backup rollback preflight refuses unrelated dirty tracked files", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    GitFixture::write_file(f.project + "/unrelated.txt", "clean\n");
    REQUIRE(std::system(("cd '" + f.project + "' && git add unrelated.txt && git commit -qm 'add unrelated'").c_str()) == 0);

    auto captured = TroubleshootBackup::capture(f.project, "run-dirty", "test", f.session_dir);
    REQUIRE(captured.ok());

    GitFixture::write_file(f.project + "/unrelated.txt", "dirt\n");
    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.error().find("unrelated.txt") != std::string::npos);
    REQUIRE(f.read_file(f.project + "/unrelated.txt") == "dirt\n");
#endif
}

TEST_CASE("Backup rollback preflight refuses branch drift", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-branch", "test", f.session_dir);
    REQUIRE(captured.ok());
    REQUIRE(std::system(("cd '" + f.project + "' && git checkout -qb feature").c_str()) == 0);

    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.error().find("feature") != std::string::npos);
    REQUIRE(report.error().find(captured.value().current_branch) != std::string::npos);
#endif
}

TEST_CASE("Backup rollback refuses partial new-format artifact deletion", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-partial-artifacts", "test",
                                                f.session_dir);
    REQUIRE(captured.ok());
    REQUIRE(platform::file_exists(f.session_dir + "/pre-modified.txt"));
    REQUIRE(platform::file_exists(f.session_dir + "/agent-modified.txt"));
    REQUIRE(std::remove((f.session_dir + "/current-branch.txt").c_str()) == 0);

    GitFixture::write_file(f.project + "/tracked.txt", "operator dirty should remain\n");
    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.error().find("current-branch.txt") != std::string::npos);
    REQUIRE(report.error().find("incomplete troubleshoot session artifacts") != std::string::npos);
    REQUIRE(f.read_file(f.project + "/tracked.txt") == "operator dirty should remain\n");
#endif
}

TEST_CASE("Backup rollback permits dirty files that were pre-modified or agent-touched", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    GitFixture::write_file(f.project + "/tracked.txt", "tracked-second-baseline\n");
    REQUIRE(std::system(("cd '" + f.project + "' && git add tracked.txt && git commit -qm 'second baseline'").c_str()) == 0);

    GitFixture::write_file(f.project + "/tracked.txt", "operator dirty before capture\n");
    auto captured = TroubleshootBackup::capture(f.project, "run-overlap", "test", f.session_dir);
    REQUIRE(captured.ok());

    GitFixture::write_file(f.project + "/tracked.txt", "agent committed edit\n");
    REQUIRE(std::system(("cd '" + f.project + "' && git add tracked.txt && git commit -qm 'agent edit tracked'").c_str()) == 0);
    GitFixture::write_file(f.project + "/tracked.txt", "agent dirty follow-up\n");

    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE(report.ok());
    REQUIRE(f.read_file(f.project + "/tracked.txt") == "tracked-second-baseline\n");
#endif
}

TEST_CASE("Backup rollback succeeds when agent makes working-tree-only edits", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-wt-agent", "test", f.session_dir);
    REQUIRE(captured.ok());

    GitFixture::write_file(f.project + "/tracked.txt", "agent dirty\n");
    auto touched = TroubleshootBackup::record_agent_touch(
        f.project, captured.value().base_sha, f.session_dir);
    REQUIRE(touched.ok());
    REQUIRE(touched.value().size() == 1);
    REQUIRE(touched.value()[0] == "tracked.txt");
    REQUIRE(platform::file_exists(f.session_dir + "/agent-modified.txt"));

    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE(report.ok());
    REQUIRE(f.read_file(f.project + "/tracked.txt") == "tracked-original\n");
#endif
}

TEST_CASE("Backup rollback refuses when operator dirties unrelated file after agent exit", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    GitFixture::write_file(f.project + "/unrelated.txt", "clean\n");
    REQUIRE(std::system(("cd '" + f.project + "' && git add unrelated.txt && git commit -qm 'add unrelated'").c_str()) == 0);

    auto captured = TroubleshootBackup::capture(f.project, "run-post-agent-drift", "test", f.session_dir);
    REQUIRE(captured.ok());

    GitFixture::write_file(f.project + "/tracked.txt", "agent dirty\n");
    auto touched = TroubleshootBackup::record_agent_touch(
        f.project, captured.value().base_sha, f.session_dir);
    REQUIRE(touched.ok());
    REQUIRE(touched.value().size() == 1);
    REQUIRE(touched.value()[0] == "tracked.txt");

    GitFixture::write_file(f.project + "/unrelated.txt", "operator dirty after agent\n");
    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.error().find("unrelated.txt") != std::string::npos);
    REQUIRE(f.read_file(f.project + "/tracked.txt") == "agent dirty\n");
    REQUIRE(f.read_file(f.project + "/unrelated.txt") == "operator dirty after agent\n");
#endif
}

TEST_CASE("Backup rollback round-trips detached HEAD state", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    REQUIRE(std::system(("cd '" + f.project + "' && git checkout -q --detach HEAD").c_str()) == 0);
    GitFixture::write_file(f.project + "/tracked.txt", "operator dirty detached\n");

    auto captured = TroubleshootBackup::capture(f.project, "run-detached", "test", f.session_dir);
    REQUIRE(captured.ok());
    REQUIRE(captured.value().current_branch.find("__detached__:") == 0);

    GitFixture::write_file(f.project + "/tracked.txt", "agent dirty detached\n");
    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE(report.ok());
    REQUIRE(report.value().current_branch == captured.value().current_branch);
    REQUIRE(f.read_file(f.project + "/tracked.txt") == "tracked-original\n");
#endif
}

TEST_CASE("Backup rollback validates recorded base SHA", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-sha", "test", f.session_dir);
    REQUIRE(captured.ok());
    const std::string original = f.read_file(f.session_dir + "/backup-base.txt");

    GitFixture::write_file(f.session_dir + "/backup-base.txt", "not-a-sha\n");
    auto bad = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.error().find("SHA validation") != std::string::npos);

    GitFixture::write_file(f.session_dir + "/backup-base.txt", original);
    auto good = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE(good.ok());
#endif
}

TEST_CASE("Backup capture rejects non-git directories", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    std::string root = platform::temp_dir() + "/needle_backup_no_git_" + std::to_string(getpid());
    platform::remove_recursive(root);
    std::string project = root + "/not-a-repo";
    std::string session_dir = root + "/run/troubleshoot/session-test";
    platform::mkdir_p(project);
    platform::mkdir_p(session_dir);

    auto captured = TroubleshootBackup::capture(project, "run-x", "test", session_dir);
    REQUIRE_FALSE(captured.ok());
    REQUIRE(captured.error().find("git repository") != std::string::npos);

    platform::remove_recursive(root);
#endif
}

TEST_CASE("Backup rollback refuses when no backup-base.txt exists", "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    // Don't call capture(); session dir has no backup-base.txt.
    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.error().find("backup-base.txt") != std::string::npos);
#endif
}

TEST_CASE("Backup rollback legacy session fallback succeeds on clean tree (m-e)",
          "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-legacy", "test", f.session_dir);
    REQUIRE(captured.ok());
    // Simulate a legacy (pre-SPRINT-017) session by removing artifacts that
    // didn't exist before this revision.
    REQUIRE(std::remove((f.session_dir + "/current-branch.txt").c_str()) == 0);
    REQUIRE(std::remove((f.session_dir + "/pre-modified.txt").c_str()) == 0);
    // agent-modified.txt is written by record_agent_touch (not capture), so
    // its absence here is normal — tolerate either state for legacy fixtures.
    std::remove((f.session_dir + "/agent-modified.txt").c_str());

    // Clean working tree → rollback should succeed.
    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE(report.ok());
#endif
}

TEST_CASE("Backup rollback legacy session refuses dirty tree (m-e)",
          "[troubleshoot][backup]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    GitFixture f;
    auto captured = TroubleshootBackup::capture(f.project, "run-legacy-dirty", "test",
                                                f.session_dir);
    REQUIRE(captured.ok());
    REQUIRE(std::remove((f.session_dir + "/current-branch.txt").c_str()) == 0);
    REQUIRE(std::remove((f.session_dir + "/pre-modified.txt").c_str()) == 0);
    // agent-modified.txt is written by record_agent_touch (not capture), so
    // its absence here is normal — tolerate either state for legacy fixtures.
    std::remove((f.session_dir + "/agent-modified.txt").c_str());

    // Dirty file with no recorded baseline → refuse.
    GitFixture::write_file(f.project + "/tracked.txt", "dirty after legacy capture\n");
    auto report = TroubleshootBackup::rollback(f.project, f.session_dir);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.error().find("legacy session") != std::string::npos);
    REQUIRE(f.read_file(f.project + "/tracked.txt") == "dirty after legacy capture\n");
#endif
}
