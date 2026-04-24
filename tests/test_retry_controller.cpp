#include <catch2/catch.hpp>
#include "needle/engine/retry_controller.h"

using namespace needle;

TEST_CASE("RetryController: initial attempts is zero", "[retry]") {
    RetryController rc;
    REQUIRE(rc.attempts("node1") == 0);
}

TEST_CASE("RetryController: record_attempt increments count", "[retry]") {
    RetryController rc;
    rc.record_attempt("node1");
    REQUIRE(rc.attempts("node1") == 1);

    rc.record_attempt("node1");
    REQUIRE(rc.attempts("node1") == 2);
}

TEST_CASE("RetryController: should_retry respects policy limit", "[retry]") {
    RetryController rc;
    RetryPolicy policy;
    policy.max_retries = 3;
    policy.base_delay_ms = 100;
    policy.multiplier = 1.0;
    policy.max_delay_ms = 1000;
    policy.jitter = false;

    REQUIRE(rc.should_retry("node1", policy) == true);

    rc.record_attempt("node1");
    REQUIRE(rc.should_retry("node1", policy) == true);

    rc.record_attempt("node1");
    REQUIRE(rc.should_retry("node1", policy) == true);

    rc.record_attempt("node1");
    REQUIRE(rc.should_retry("node1", policy) == false);
}

TEST_CASE("RetryController: independent tracking per node", "[retry]") {
    RetryController rc;
    RetryPolicy policy;
    policy.max_retries = 2;
    policy.base_delay_ms = 100;
    policy.multiplier = 1.0;
    policy.max_delay_ms = 1000;
    policy.jitter = false;

    rc.record_attempt("A");
    rc.record_attempt("A");
    rc.record_attempt("B");

    REQUIRE(rc.attempts("A") == 2);
    REQUIRE(rc.attempts("B") == 1);
    REQUIRE(rc.should_retry("A", policy) == false);
    REQUIRE(rc.should_retry("B", policy) == true);
}

TEST_CASE("RetryController: JSON round-trip", "[retry]") {
    RetryController rc;
    rc.record_attempt("node1");
    rc.record_attempt("node1");
    rc.record_attempt("node2");

    nlohmann::json j = rc.to_json();
    RetryController rc2 = RetryController::from_json(j);

    REQUIRE(rc2.attempts("node1") == 2);
    REQUIRE(rc2.attempts("node2") == 1);
    REQUIRE(rc2.attempts("node3") == 0);
}

TEST_CASE("RetryController: zero max_retries means no retries allowed", "[retry]") {
    RetryController rc;
    RetryPolicy policy;
    policy.max_retries = 0;
    policy.base_delay_ms = 100;
    policy.multiplier = 1.0;
    policy.max_delay_ms = 1000;
    policy.jitter = false;

    REQUIRE(rc.should_retry("node1", policy) == false);
}
