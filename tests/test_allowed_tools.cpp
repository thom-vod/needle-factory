#include <catch2/catch.hpp>

#include "needle/troubleshoot/allowed_tools.h"

using namespace needle;

TEST_CASE("allowed tools off is empty", "[allowed_tools]") {
    REQUIRE(build_allowed_tools(TroubleshootMode::Off, "/project", "/project/g.dot", "/run") == "");
}

TEST_CASE("allowed tools diagnose includes Read + escalate Bash", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Diagnose,
                                            "/project", "/project/g.dot", "/run/recovery");
    REQUIRE(tools.find("Read Glob Grep") != std::string::npos);
    REQUIRE(tools.find("Write(recovery.md)") != std::string::npos);
    REQUIRE(tools.find("Write(agent.stdout.log)") != std::string::npos);
    REQUIRE(tools.find("Write(agent.stderr.log)") != std::string::npos);
    // SPRINT-016 B2 fix: escalate must be reachable from Diagnose.
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
    // Diagnose must not allow project edits.
    REQUIRE(tools.find("Edit(") == std::string::npos);
}

TEST_CASE("allowed tools tweak emits relative-glob patterns", "[allowed_tools]") {
    // SPRINT-016 Phase 0 finding: absolute-path Edit patterns don't match
    // in claude's allow-list parser. We pass the graph path verbatim
    // (callers should pre-relativise) and use relative globs for
    // recursive patterns. The agent is invoked with cwd=project_dir.
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project", "graph.dot", "/run/recovery");
    REQUIRE(tools.find("Edit(graph.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(.needle/**/stages/*/prompt.md)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle stage mark:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle resume:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(git diff:*)") != std::string::npos);
    // SPRINT-016 B2 fix.
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
}

TEST_CASE("allowed tools tweak falls back to *.dot when graph path is empty", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project", "", "/run/recovery");
    REQUIRE(tools.find("Edit(*.dot)") != std::string::npos);
}

TEST_CASE("allowed tools full uses broader allow-list (no dangerously-skip-permissions)", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Full, "/project", "graph.dot", "/run");
    // SPRINT-016 spec revision: Full no longer uses --dangerously-skip-permissions.
    REQUIRE(tools.find("--dangerously-skip-permissions") == std::string::npos);
    // It does broaden Edit/Write and the Bash allow-list.
    REQUIRE(tools.find("Edit(**)") != std::string::npos);
    REQUIRE(tools.find("Write(**)") != std::string::npos);
    REQUIRE(tools.find("Bash(npm install:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(cargo build:*)") != std::string::npos);
    // Tweak's allow-list is still included.
    REQUIRE(tools.find("Edit(graph.dot)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
}
