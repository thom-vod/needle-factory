#include <catch2/catch.hpp>

#include "needle/troubleshoot/allowed_tools.h"

using namespace needle;

namespace {

// 2026-05-18 empirical probe (claude 2.1.144): every parenthesised
// `Write(...)` pattern is denied at the permission prompt — only bare
// `Write` is accepted. We assert the bare form here. Scoping shifted to
// the file-write hook post-run audit. See allowed_tools.cpp note above
// kBareWrite.
void require_bare_write(const std::string& tools) {
    // Must contain the bare token surrounded by word boundaries (space or
    // end-of-string). Reject the parenthesised variants explicitly.
    REQUIRE(tools.find(" Write ") != std::string::npos);
    REQUIRE(tools.find("Write(**/recovery.md)") == std::string::npos);
    REQUIRE(tools.find("Write(**)") == std::string::npos);
}

} // namespace

TEST_CASE("allowed tools off is empty", "[allowed_tools]") {
    REQUIRE(build_allowed_tools(TroubleshootMode::Off, "/project", "/project/g.dot", "/run") == "");
}

TEST_CASE("allowed tools diagnose includes Read + escalate Bash", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Diagnose,
                                            "/project",
                                            "/project/g.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Read Glob Grep") != std::string::npos);
    require_bare_write(tools);
    // SPRINT-016 B2 fix: escalate must be reachable from Diagnose.
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
    // Diagnose must not allow project edits.
    REQUIRE(tools.find("Edit(") == std::string::npos);
}

TEST_CASE("allowed tools emit the same session artifact writes inside and outside project", "[allowed_tools]") {
    std::string inside = build_allowed_tools(TroubleshootMode::Diagnose,
                                             "/project",
                                             "/project/g.dot",
                                             "/project/.needle/run-x/troubleshoot/session-y");
    std::string outside = build_allowed_tools(TroubleshootMode::Diagnose,
                                              "/project",
                                              "/project/g.dot",
                                              "/tmp/.needle/run-x/troubleshoot/session-y");
    REQUIRE(inside == outside);
    require_bare_write(inside);
    require_bare_write(outside);
}

TEST_CASE("allowed tools tweak emits relative edit patterns", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "graph.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(.needle/**/source.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(*.dot)") != std::string::npos);
    require_bare_write(tools);
    REQUIRE(tools.find("Edit(.needle/**/stages/*/prompt.md)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle stage mark:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle resume:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(git diff:*)") != std::string::npos);
    // SPRINT-016 B2 fix.
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
}

TEST_CASE("allowed tools tweak keeps basename dot fallback when graph path is empty", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(.needle/**/source.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(*.dot)") != std::string::npos);
}

TEST_CASE("allowed tools tweak ignores original graph path inside project", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "/project/sub/graph.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(.needle/**/source.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(*.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(sub/graph.dot)") == std::string::npos);
    REQUIRE(tools.find("Edit(/project/sub/graph.dot)") == std::string::npos);
}

TEST_CASE("allowed tools tweak ignores original graph path outside project", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project",
                                            "/outside/graph.dot",
                                            "/project/.needle/run-x/troubleshoot/session-y");
    REQUIRE(tools.find("Edit(.needle/**/source.dot)") != std::string::npos);
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
    // Full broadens Edit only; Write is bare in the base allow-list
    // (parenthesised Write is denied by claude 2.1.144).
    REQUIRE(tools.find("Edit(**)") != std::string::npos);
    require_bare_write(tools);
    REQUIRE(tools.find("Bash(npm install:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(cargo build:*)") != std::string::npos);
    // Tweak's allow-list is still included.
    REQUIRE(tools.find("Edit(.needle/**/source.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(*.dot)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle troubleshoot escalate:*)") != std::string::npos);
}
