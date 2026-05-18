#include <catch2/catch.hpp>

#include "needle/troubleshoot/allowed_tools.h"

using namespace needle;

TEST_CASE("allowed tools off is empty", "[allowed_tools]") {
    REQUIRE(build_allowed_tools(TroubleshootMode::Off, "/project", "/project/g.dot", "/run") == "");
}

TEST_CASE("allowed tools diagnose includes Read + escalate Bash", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Diagnose,
                                            "/project",
                                            "/project/g.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Read Glob Grep") != std::string::npos);
    REQUIRE(tools.find("Write(.needle/run-x/troubleshoot/session-y/recovery.md)") != std::string::npos);
    REQUIRE(tools.find("Write(.needle/run-x/troubleshoot/session-y/agent.stdout.log)") != std::string::npos);
    REQUIRE(tools.find("Write(.needle/run-x/troubleshoot/session-y/agent.stderr.log)") != std::string::npos);
    // SPRINT-016 B2 fix: escalate must be reachable from Diagnose.
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
    // Diagnose must not allow project edits.
    REQUIRE(tools.find("Edit(") == std::string::npos);
}

TEST_CASE("allowed tools tweak emits relative-glob patterns", "[allowed_tools]") {
    // SPRINT-016 Phase 0 finding: absolute-path Edit patterns don't match
    // in claude's allow-list parser. We emit relative globs for
    // recursive patterns. The agent is invoked with cwd=project_dir.
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "graph.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(graph.dot)") != std::string::npos);
    REQUIRE(tools.find("Write(.needle/run-x/troubleshoot/session-y/recovery.md)") != std::string::npos);
    REQUIRE(tools.find("Edit(.needle/**/stages/*/prompt.md)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle stage mark:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle resume:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(git diff:*)") != std::string::npos);
    // SPRINT-016 B2 fix.
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
}

TEST_CASE("allowed tools tweak falls back to *.dot when graph path is empty", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(*.dot)") != std::string::npos);
}

TEST_CASE("allowed tools tweak relativises absolute graph path inside project", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "/project/sub/graph.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(sub/graph.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(/project/sub/graph.dot)") == std::string::npos);
}

TEST_CASE("allowed tools tweak falls back when absolute graph path is outside project", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "/outside/graph.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(*.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(/outside/graph.dot)") == std::string::npos);
}

TEST_CASE("allowed tools full uses broader allow-list (no dangerously-skip-permissions)", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Full,
                                            "/project",
                                            "graph.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    // SPRINT-016 spec revision: Full no longer uses --dangerously-skip-permissions.
    REQUIRE(tools.find("--dangerously-skip-permissions") == std::string::npos);
    // It does broaden Edit/Write and the Bash allow-list.
    REQUIRE(tools.find("Edit(**)") != std::string::npos);
    REQUIRE(tools.find("Write(**)") != std::string::npos);
    REQUIRE(tools.find("Write(.needle/run-x/troubleshoot/session-y/recovery.md)") != std::string::npos);
    REQUIRE(tools.find("Bash(npm install:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(cargo build:*)") != std::string::npos);
    // Tweak's allow-list is still included.
    REQUIRE(tools.find("Edit(graph.dot)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
}
