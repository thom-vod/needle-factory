#include <catch2/catch.hpp>
#include "needle/model/retry_policy.h"

using namespace needle;

TEST_CASE("RetryPolicy default is STANDARD", "[retry]") {
    RetryPolicy p = RetryPolicy::default_policy();
    REQUIRE(p.max_retries == 5);
    REQUIRE(p.base_delay_ms == 200);
    REQUIRE(p.multiplier == Approx(2.0));
    REQUIRE(p.jitter == true);
}

TEST_CASE("RetryPolicy STANDARD preset", "[retry]") {
    RetryPolicy p = RetryPolicy::from_preset(RetryPreset::STANDARD);
    REQUIRE(p.max_retries == 5);
    REQUIRE(p.base_delay_ms == 200);
    REQUIRE(p.multiplier == Approx(2.0));
}

TEST_CASE("RetryPolicy AGGRESSIVE preset", "[retry]") {
    RetryPolicy p = RetryPolicy::from_preset(RetryPreset::AGGRESSIVE);
    REQUIRE(p.max_retries == 10);
    REQUIRE(p.base_delay_ms == 100);
    REQUIRE(p.multiplier == Approx(1.5));
}

TEST_CASE("RetryPolicy LINEAR preset", "[retry]") {
    RetryPolicy p = RetryPolicy::from_preset(RetryPreset::LINEAR);
    REQUIRE(p.max_retries == 5);
    REQUIRE(p.base_delay_ms == 1000);
    REQUIRE(p.multiplier == Approx(1.0));
    REQUIRE(p.jitter == false);
}

TEST_CASE("RetryPolicy PATIENT preset", "[retry]") {
    RetryPolicy p = RetryPolicy::from_preset(RetryPreset::PATIENT);
    REQUIRE(p.max_retries == 3);
    REQUIRE(p.base_delay_ms == 5000);
    REQUIRE(p.multiplier == Approx(3.0));
}

TEST_CASE("RetryPolicy delay_for_attempt without jitter", "[retry]") {
    RetryPolicy p;
    p.max_retries = 5;
    p.base_delay_ms = 100;
    p.multiplier = 2.0;
    p.max_delay_ms = 10000;
    p.jitter = false;

    REQUIRE(p.delay_for_attempt(0) == 100);   // 100 * 2^0
    REQUIRE(p.delay_for_attempt(1) == 200);   // 100 * 2^1
    REQUIRE(p.delay_for_attempt(2) == 400);   // 100 * 2^2
    REQUIRE(p.delay_for_attempt(3) == 800);   // 100 * 2^3
}

TEST_CASE("RetryPolicy delay_for_attempt respects max_delay", "[retry]") {
    RetryPolicy p;
    p.max_retries = 5;
    p.base_delay_ms = 1000;
    p.multiplier = 10.0;
    p.max_delay_ms = 5000;
    p.jitter = false;

    // 1000 * 10^1 = 10000, capped to 5000
    REQUIRE(p.delay_for_attempt(1) == 5000);
}

TEST_CASE("RetryPolicy delay_for_attempt with jitter stays in bounds", "[retry]") {
    RetryPolicy p;
    p.max_retries = 5;
    p.base_delay_ms = 1000;
    p.multiplier = 1.0;
    p.max_delay_ms = 30000;
    p.jitter = true;

    // With jitter, delay should be between 1000 and 1250 (1000 + 25%)
    for (int i = 0; i < 100; ++i) {
        int delay = p.delay_for_attempt(0);
        REQUIRE(delay >= 1000);
        REQUIRE(delay <= 1250);
    }
}

TEST_CASE("RetryPolicy LINEAR has constant delay", "[retry]") {
    RetryPolicy p = RetryPolicy::from_preset(RetryPreset::LINEAR);

    // multiplier is 1.0 and jitter is false, so every attempt has same delay
    REQUIRE(p.delay_for_attempt(0) == 1000);
    REQUIRE(p.delay_for_attempt(1) == 1000);
    REQUIRE(p.delay_for_attempt(5) == 1000);
}

TEST_CASE("RetryPolicy from_attributes with preset", "[retry]") {
    AttributeMap attrs;
    attrs.set("retry_preset", "aggressive");
    RetryPolicy p = RetryPolicy::from_attributes(attrs);
    REQUIRE(p.max_retries == 10);
    REQUIRE(p.base_delay_ms == 100);
}

TEST_CASE("RetryPolicy from_attributes with individual values", "[retry]") {
    AttributeMap attrs;
    attrs.set("max_retries", "3");
    attrs.set("base_delay", "500ms");
    attrs.set("retry_multiplier", "1.5");
    attrs.set("retry_jitter", "false");
    RetryPolicy p = RetryPolicy::from_attributes(attrs);
    REQUIRE(p.max_retries == 3);
    REQUIRE(p.base_delay_ms == 500);
    REQUIRE(p.multiplier == Approx(1.5));
    REQUIRE(p.jitter == false);
}

TEST_CASE("RetryPolicy from_attributes defaults to STANDARD", "[retry]") {
    AttributeMap attrs;
    RetryPolicy p = RetryPolicy::from_attributes(attrs);
    REQUIRE(p.max_retries == 5);
    REQUIRE(p.base_delay_ms == 200);
}
