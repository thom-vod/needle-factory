#include <catch2/catch.hpp>
#include "needle/parser/stylesheet_parser.h"

using namespace needle;

TEST_CASE("StylesheetParser: universal selector", "[stylesheet]") {
    auto result = StylesheetParser::parse("* { llm_model: claude-sonnet-4-5; }");
    REQUIRE(result.ok());
    REQUIRE(result.value().rules.size() == 1);
    REQUIRE(result.value().rules[0].selector_type == StyleRule::UNIVERSAL);
    REQUIRE(result.value().rules[0].selector == "*");
    REQUIRE(result.value().rules[0].properties.get("llm_model") == "claude-sonnet-4-5");
}

TEST_CASE("StylesheetParser: class selector", "[stylesheet]") {
    auto result = StylesheetParser::parse(".code { llm_model: claude-opus-4-6; }");
    REQUIRE(result.ok());
    REQUIRE(result.value().rules.size() == 1);
    REQUIRE(result.value().rules[0].selector_type == StyleRule::CLASS);
    REQUIRE(result.value().rules[0].selector == ".code");
    REQUIRE(result.value().rules[0].properties.get("llm_model") == "claude-opus-4-6");
}

TEST_CASE("StylesheetParser: ID selector", "[stylesheet]") {
    auto result = StylesheetParser::parse("#critical { llm_provider: openai; }");
    REQUIRE(result.ok());
    REQUIRE(result.value().rules.size() == 1);
    REQUIRE(result.value().rules[0].selector_type == StyleRule::ID);
    REQUIRE(result.value().rules[0].selector == "#critical");
    REQUIRE(result.value().rules[0].properties.get("llm_provider") == "openai");
}

TEST_CASE("StylesheetParser: multiple rules", "[stylesheet]") {
    auto result = StylesheetParser::parse(
        "* { llm_model: claude-sonnet-4-5; }\n"
        ".code { llm_model: claude-opus-4-6; }\n"
        "#critical { llm_provider: openai; }"
    );
    REQUIRE(result.ok());
    REQUIRE(result.value().rules.size() == 3);
}

TEST_CASE("StylesheetParser: multiple properties", "[stylesheet]") {
    auto result = StylesheetParser::parse(
        "#node1 { llm_provider: openai; reasoning_effort: high; temperature: 0.5; }"
    );
    REQUIRE(result.ok());
    const auto& rule = result.value().rules[0];
    REQUIRE(rule.properties.get("llm_provider") == "openai");
    REQUIRE(rule.properties.get("reasoning_effort") == "high");
    REQUIRE(rule.properties.get("temperature") == "0.5");
}

TEST_CASE("StylesheetParser: empty input", "[stylesheet]") {
    auto result = StylesheetParser::parse("");
    REQUIRE(result.ok());
    REQUIRE(result.value().rules.empty());
}

TEST_CASE("StylesheetParser: whitespace-only input", "[stylesheet]") {
    auto result = StylesheetParser::parse("   \n\t  ");
    REQUIRE(result.ok());
    REQUIRE(result.value().rules.empty());
}

TEST_CASE("StylesheetParser: malformed - missing brace", "[stylesheet]") {
    auto result = StylesheetParser::parse("* llm_model: test; }");
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("StylesheetParser: malformed - bad selector", "[stylesheet]") {
    auto result = StylesheetParser::parse("@ { prop: val; }");
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("StylesheetParser: quoted values", "[stylesheet]") {
    auto result = StylesheetParser::parse("* { prompt: \"hello world\"; }");
    REQUIRE(result.ok());
    REQUIRE(result.value().rules[0].properties.get("prompt") == "hello world");
}
