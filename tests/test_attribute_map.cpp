#include <catch2/catch.hpp>
#include "needle/model/attribute_map.h"

using namespace needle;

TEST_CASE("AttributeMap set and get", "[attribute_map]") {
    AttributeMap m;
    m.set("key", "value");
    REQUIRE(m.get("key") == "value");
}

TEST_CASE("AttributeMap get with default", "[attribute_map]") {
    AttributeMap m;
    REQUIRE(m.get("missing") == "");
    REQUIRE(m.get("missing", "fallback") == "fallback");
}

TEST_CASE("AttributeMap has", "[attribute_map]") {
    AttributeMap m;
    REQUIRE_FALSE(m.has("key"));
    m.set("key", "val");
    REQUIRE(m.has("key"));
}

TEST_CASE("AttributeMap overwrite", "[attribute_map]") {
    AttributeMap m;
    m.set("key", "a");
    m.set("key", "b");
    REQUIRE(m.get("key") == "b");
}

TEST_CASE("AttributeMap get_int valid", "[attribute_map]") {
    AttributeMap m;
    m.set("count", "42");
    Maybe<int> val = m.get_int("count");
    REQUIRE(val.has_value());
    REQUIRE(*val == 42);
}

TEST_CASE("AttributeMap get_int invalid", "[attribute_map]") {
    AttributeMap m;
    m.set("count", "abc");
    REQUIRE_FALSE(m.get_int("count").has_value());
}

TEST_CASE("AttributeMap get_int missing", "[attribute_map]") {
    AttributeMap m;
    REQUIRE_FALSE(m.get_int("missing").has_value());
}

TEST_CASE("AttributeMap get_int rejects trailing chars", "[attribute_map]") {
    AttributeMap m;
    m.set("count", "42abc");
    REQUIRE_FALSE(m.get_int("count").has_value());
}

TEST_CASE("AttributeMap get_double", "[attribute_map]") {
    AttributeMap m;
    m.set("rate", "3.14");
    Maybe<double> val = m.get_double("rate");
    REQUIRE(val.has_value());
    REQUIRE(*val == Approx(3.14));
}

TEST_CASE("AttributeMap get_double invalid", "[attribute_map]") {
    AttributeMap m;
    m.set("rate", "not_a_number");
    REQUIRE_FALSE(m.get_double("rate").has_value());
}

TEST_CASE("AttributeMap get_bool true values", "[attribute_map]") {
    AttributeMap m;
    m.set("a", "true");
    m.set("b", "1");
    m.set("c", "yes");
    REQUIRE(*m.get_bool("a") == true);
    REQUIRE(*m.get_bool("b") == true);
    REQUIRE(*m.get_bool("c") == true);
}

TEST_CASE("AttributeMap get_bool false values", "[attribute_map]") {
    AttributeMap m;
    m.set("a", "false");
    m.set("b", "0");
    m.set("c", "no");
    REQUIRE(*m.get_bool("a") == false);
    REQUIRE(*m.get_bool("b") == false);
    REQUIRE(*m.get_bool("c") == false);
}

TEST_CASE("AttributeMap get_bool invalid", "[attribute_map]") {
    AttributeMap m;
    m.set("a", "maybe");
    REQUIRE_FALSE(m.get_bool("a").has_value());
}

TEST_CASE("AttributeMap get_duration_ms plain number", "[attribute_map]") {
    AttributeMap m;
    m.set("delay", "500");
    REQUIRE(*m.get_duration_ms("delay") == 500);
}

TEST_CASE("AttributeMap get_duration_ms with ms suffix", "[attribute_map]") {
    AttributeMap m;
    m.set("delay", "200ms");
    REQUIRE(*m.get_duration_ms("delay") == 200);
}

TEST_CASE("AttributeMap get_duration_ms with s suffix", "[attribute_map]") {
    AttributeMap m;
    m.set("delay", "2s");
    REQUIRE(*m.get_duration_ms("delay") == 2000);
}

TEST_CASE("AttributeMap get_duration_ms with m suffix", "[attribute_map]") {
    AttributeMap m;
    m.set("delay", "1m");
    REQUIRE(*m.get_duration_ms("delay") == 60000);
}

TEST_CASE("AttributeMap get_duration_ms missing", "[attribute_map]") {
    AttributeMap m;
    REQUIRE_FALSE(m.get_duration_ms("delay").has_value());
}

TEST_CASE("AttributeMap raw", "[attribute_map]") {
    AttributeMap m;
    m.set("a", "1");
    m.set("b", "2");
    const auto& raw = m.raw();
    REQUIRE(raw.size() == 2);
    REQUIRE(raw.at("a") == "1");
    REQUIRE(raw.at("b") == "2");
}
