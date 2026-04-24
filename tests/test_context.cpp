#include <catch2/catch.hpp>
#include "needle/model/context.h"

using namespace needle;

TEST_CASE("Context set and get", "[context]") {
    Context ctx;
    ctx.set("name", "alice");
    REQUIRE(ctx.get("name") == "alice");
}

TEST_CASE("Context get missing key returns empty", "[context]") {
    Context ctx;
    REQUIRE(ctx.get("missing") == "");
}

TEST_CASE("Context has", "[context]") {
    Context ctx;
    REQUIRE_FALSE(ctx.has("key"));
    ctx.set("key", "val");
    REQUIRE(ctx.has("key"));
}

TEST_CASE("Context apply_updates", "[context]") {
    Context ctx;
    ctx.set("a", "1");

    std::map<std::string, std::string> updates;
    updates["a"] = "updated";
    updates["b"] = "new";
    ctx.apply_updates(updates);

    REQUIRE(ctx.get("a") == "updated");
    REQUIRE(ctx.get("b") == "new");
}

TEST_CASE("Context clone is independent", "[context]") {
    Context ctx;
    ctx.set("key", "original");

    Context cloned = ctx.clone();
    cloned.set("key", "modified");
    cloned.set("new_key", "new_val");

    REQUIRE(ctx.get("key") == "original");
    REQUIRE_FALSE(ctx.has("new_key"));
    REQUIRE(cloned.get("key") == "modified");
    REQUIRE(cloned.get("new_key") == "new_val");
}

TEST_CASE("Context JSON round-trip", "[context]") {
    Context ctx;
    ctx.set("name", "test");
    ctx.set("count", "42");

    nlohmann::json j = ctx.to_json();
    Context restored = Context::from_json(j);

    REQUIRE(restored.get("name") == "test");
    REQUIRE(restored.get("count") == "42");
}

TEST_CASE("Context JSON empty round-trip", "[context]") {
    Context ctx;
    nlohmann::json j = ctx.to_json();
    Context restored = Context::from_json(j);
    REQUIRE(restored.get("anything") == "");
}

TEST_CASE("Context overwrite value", "[context]") {
    Context ctx;
    ctx.set("key", "first");
    ctx.set("key", "second");
    REQUIRE(ctx.get("key") == "second");
}
