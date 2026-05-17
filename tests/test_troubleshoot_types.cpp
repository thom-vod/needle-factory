#include <catch2/catch.hpp>

#include "needle/troubleshoot/types.h"

using namespace needle;

TEST_CASE("TroubleshootMode string conversions round-trip", "[troubleshoot][types]") {
    const TroubleshootMode values[] = {
        TroubleshootMode::Off,
        TroubleshootMode::Diagnose,
        TroubleshootMode::Tweak,
        TroubleshootMode::Full,
    };

    for (TroubleshootMode value : values) {
        Maybe<TroubleshootMode> parsed = parse_troubleshoot_mode(to_string(value));
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == value);
    }
}

TEST_CASE("TroubleshootMode parser accepts legacy booleans", "[troubleshoot][types]") {
    Maybe<TroubleshootMode> true_mode = parse_troubleshoot_mode("true");
    REQUIRE(true_mode.has_value());
    REQUIRE(*true_mode == TroubleshootMode::Tweak);

    Maybe<TroubleshootMode> false_mode = parse_troubleshoot_mode("false");
    REQUIRE(false_mode.has_value());
    REQUIRE(*false_mode == TroubleshootMode::Off);
}

TEST_CASE("TroubleshootMode parser rejects unknown strings", "[troubleshoot][types]") {
    REQUIRE_FALSE(parse_troubleshoot_mode("").has_value());
    REQUIRE_FALSE(parse_troubleshoot_mode("yes").has_value());
}

TEST_CASE("TroubleshootSessionStatus string conversions cover every value", "[troubleshoot][types]") {
    REQUIRE(to_string(TroubleshootSessionStatus::Running) == "running");
    REQUIRE(to_string(TroubleshootSessionStatus::Resumed) == "resumed");
    REQUIRE(to_string(TroubleshootSessionStatus::Reported) == "reported");
    REQUIRE(to_string(TroubleshootSessionStatus::Escalated) == "escalated");
    REQUIRE(to_string(TroubleshootSessionStatus::Cancelled) == "cancelled");
    REQUIRE(to_string(TroubleshootSessionStatus::FailedKilledBudget) == "failed_killed_budget");
    REQUIRE(to_string(TroubleshootSessionStatus::FailedTimeout) == "failed_timeout");
    REQUIRE(to_string(TroubleshootSessionStatus::FailedAgent) == "failed_agent");
}

TEST_CASE("TroubleshootSessionStatus parser round-trips", "[troubleshoot][types]") {
    const TroubleshootSessionStatus values[] = {
        TroubleshootSessionStatus::Running,
        TroubleshootSessionStatus::Resumed,
        TroubleshootSessionStatus::Reported,
        TroubleshootSessionStatus::Escalated,
        TroubleshootSessionStatus::Cancelled,
        TroubleshootSessionStatus::FailedKilledBudget,
        TroubleshootSessionStatus::FailedTimeout,
        TroubleshootSessionStatus::FailedAgent,
    };
    for (TroubleshootSessionStatus value : values) {
        auto parsed = parse_troubleshoot_session_status(to_string(value));
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == value);
    }
    REQUIRE_FALSE(parse_troubleshoot_session_status("unknown").has_value());
}
