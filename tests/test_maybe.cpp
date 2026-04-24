#include <catch2/catch.hpp>
#include "needle/model/maybe.h"
#include <string>

using namespace needle;

TEST_CASE("Maybe<int> default is empty", "[maybe]") {
    Maybe<int> m;
    REQUIRE_FALSE(m.has_value());
    REQUIRE_FALSE(m);
}

TEST_CASE("Maybe<int> with value", "[maybe]") {
    Maybe<int> m(42);
    REQUIRE(m.has_value());
    REQUIRE(m);
    REQUIRE(*m == 42);
}

TEST_CASE("Maybe<string> with value", "[maybe]") {
    Maybe<std::string> m(std::string("hello"));
    REQUIRE(m.has_value());
    REQUIRE(*m == "hello");
}

TEST_CASE("Maybe<int> mutable dereference", "[maybe]") {
    Maybe<int> m(10);
    *m = 20;
    REQUIRE(*m == 20);
}

TEST_CASE("Maybe<double> empty", "[maybe]") {
    Maybe<double> m;
    REQUIRE_FALSE(m.has_value());
}

TEST_CASE("Maybe<bool> with value", "[maybe]") {
    Maybe<bool> m(true);
    REQUIRE(m.has_value());
    REQUIRE(*m == true);
}
