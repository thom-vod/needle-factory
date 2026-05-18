#include <catch2/catch.hpp>

#include "needle/engine/run_guard.h"

using namespace needle;

TEST_CASE("RunGuard try_reserve releases slot on destruction", "[run_guard]") {
    const std::string run_id = "run_guard_test_try_reserve";
    RunGuard::release(run_id);

    {
        auto first = RunGuard::try_reserve(run_id);
        REQUIRE(first.has_value());

        auto second = RunGuard::try_reserve(run_id);
        REQUIRE_FALSE(second.has_value());
    }

    auto third = RunGuard::try_reserve(run_id);
    REQUIRE(third.has_value());
}
