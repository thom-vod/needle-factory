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
