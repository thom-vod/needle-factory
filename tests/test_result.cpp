#include <catch2/catch.hpp>
#include "needle/model/result.h"

using namespace needle;

TEST_CASE("Result<int> success", "[result]") {
    auto r = Result<int>::success(42);
    REQUIRE(r.ok());
    REQUIRE(r);
    REQUIRE(r.value() == 42);
}

TEST_CASE("Result<int> failure", "[result]") {
    auto r = Result<int>::failure("something went wrong");
    REQUIRE_FALSE(r.ok());
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == "something went wrong");
}

TEST_CASE("Result<string> success", "[result]") {
    auto r = Result<std::string>::success("hello");
    REQUIRE(r.ok());
    REQUIRE(r.value() == "hello");
}

TEST_CASE("Result<string> failure", "[result]") {
    auto r = Result<std::string>::failure("bad");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error() == "bad");
}

TEST_CASE("Result<int> mutable value", "[result]") {
    auto r = Result<int>::success(10);
    r.value() = 20;
    REQUIRE(r.value() == 20);
}

TEST_CASE("Result<void> success", "[result]") {
    auto r = Result<void>::success();
    REQUIRE(r.ok());
    REQUIRE(r);
}

TEST_CASE("Result<void> failure", "[result]") {
    auto r = Result<void>::failure("oops");
    REQUIRE_FALSE(r.ok());
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == "oops");
}
