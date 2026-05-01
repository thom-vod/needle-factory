#include <catch2/catch.hpp>
#include "needle/worktree/strategy.h"

using namespace needle;

TEST_CASE("WorktreeStrategy: enum round-trips through string", "[worktree]") {
    REQUIRE(worktree_strategy_from_string("off") == WorktreeStrategy::Off);
    REQUIRE(worktree_strategy_from_string("Off") == WorktreeStrategy::Off);
    REQUIRE(worktree_strategy_from_string("auto") == WorktreeStrategy::Auto);
    REQUIRE(worktree_strategy_from_string("AUTO") == WorktreeStrategy::Auto);
    REQUIRE(worktree_strategy_from_string("manual") == WorktreeStrategy::Manual);
    REQUIRE(worktree_strategy_from_string("garbage") == WorktreeStrategy::Off);

    REQUIRE(to_string(WorktreeStrategy::Off) == "off");
    REQUIRE(to_string(WorktreeStrategy::Auto) == "auto");
    REQUIRE(to_string(WorktreeStrategy::Manual) == "manual");
}

TEST_CASE("interpolate_template: substitutes single variable", "[worktree]") {
    auto r = interpolate_template("auto/${run_id}", {{"run_id", "abc123"}});
    REQUIRE(r.ok());
    REQUIRE(r.value() == "auto/abc123");
}

TEST_CASE("interpolate_template: substitutes multiple variables", "[worktree]") {
    auto r = interpolate_template("../${repo}-wt-${run_id}",
                                  {{"repo", "needle"}, {"run_id", "x42"}});
    REQUIRE(r.ok());
    REQUIRE(r.value() == "../needle-wt-x42");
}

TEST_CASE("interpolate_template: missing parameter fails fast", "[worktree]") {
    auto r = interpolate_template("auto/${pbi_id}", {{"run_id", "x"}});
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().find("pbi_id") != std::string::npos);
}

TEST_CASE("interpolate_template: literal template with no vars passes through",
          "[worktree]") {
    auto r = interpolate_template("auto/static", {});
    REQUIRE(r.ok());
    REQUIRE(r.value() == "auto/static");
}

TEST_CASE("interpolate_template: unterminated `${` returns error", "[worktree]") {
    auto r = interpolate_template("auto/${run_id", {{"run_id", "x"}});
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().find("unterminated") != std::string::npos);
}
