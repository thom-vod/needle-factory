#include <catch2/catch.hpp>

#include "needle/troubleshoot/allowed_tools.h"

using namespace needle;

TEST_CASE("allowed tools off is empty", "[allowed_tools]") {
    REQUIRE(build_allowed_tools(TroubleshootMode::Off, "/project", "/project/g.dot", "/run") == "");
}

TEST_CASE("allowed tools diagnose scopes writes to recovery directory", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Diagnose,
                                            "/project", "/project/g.dot", "/run/recovery");
    REQUIRE(tools.find("Read Glob Grep") != std::string::npos);
    REQUIRE(tools.find("Write(/run/recovery/recovery.md)") != std::string::npos);
    REQUIRE(tools.find("Write(/run/recovery/agent.stdout.log)") != std::string::npos);
    REQUIRE(tools.find("Write(/run/recovery/agent.stderr.log)") != std::string::npos);
    REQUIRE(tools.find("Edit(") == std::string::npos);
}

TEST_CASE("allowed tools tweak interpolates project graph and recovery paths", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project", "/project/graph.dot", "/run/recovery");
    REQUIRE(tools.find("Edit(/project/graph.dot)") != std::string::npos);
    REQUIRE(tools.find("Edit(/project/.needle/**/stages/*/prompt.md)") != std::string::npos);
    REQUIRE(tools.find("Write(/run/recovery/snapshot/*)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle stage mark:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(needle resume:*)") != std::string::npos);
    REQUIRE(tools.find("Bash(git diff:*)") != std::string::npos);
}

TEST_CASE("allowed tools quotes paths containing spaces", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/tmp/my project", "/tmp/my project/graph.dot",
                                            "/tmp/my run/recovery");
    REQUIRE(tools.find("Edit('/tmp/my project/graph.dot')") != std::string::npos);
    REQUIRE(tools.find("Write('/tmp/my run/recovery/recovery.md')") != std::string::npos);
}

TEST_CASE("allowed tools tweak falls back when graph path is empty", "[allowed_tools]") {
    std::string tools = build_allowed_tools(TroubleshootMode::Tweak,
                                            "/project", "", "/run/recovery");
    REQUIRE(tools.find("Edit(/project/*.dot)") != std::string::npos);
}

TEST_CASE("allowed tools full returns permissions sentinel", "[allowed_tools]") {
    REQUIRE(build_allowed_tools(TroubleshootMode::Full, "/project", "/project/g.dot", "/run") ==
            "--dangerously-skip-permissions");
}
