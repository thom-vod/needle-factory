#include <catch2/catch.hpp>
#include "needle/parser/condition_parser.h"

using namespace needle;

TEST_CASE("ConditionParser: simple equality", "[condition]") {
    auto result = ConditionParser::parse("outcome=success");
    REQUIRE(result.ok());
    REQUIRE(result.value().clauses.size() == 1);
    REQUIRE(result.value().clauses[0].variable == "outcome");
    REQUIRE(result.value().clauses[0].op == "=");
    REQUIRE(result.value().clauses[0].value == "success");
}

TEST_CASE("ConditionParser: inequality", "[condition]") {
    auto result = ConditionParser::parse("outcome!=failure");
    REQUIRE(result.ok());
    REQUIRE(result.value().clauses.size() == 1);
    REQUIRE(result.value().clauses[0].op == "!=");
    REQUIRE(result.value().clauses[0].value == "failure");
}

TEST_CASE("ConditionParser: conjunction", "[condition]") {
    auto result = ConditionParser::parse("outcome=success && preferred_label=Yes");
    REQUIRE(result.ok());
    REQUIRE(result.value().clauses.size() == 2);
    REQUIRE(result.value().clauses[0].variable == "outcome");
    REQUIRE(result.value().clauses[0].value == "success");
    REQUIRE(result.value().clauses[1].variable == "preferred_label");
    REQUIRE(result.value().clauses[1].value == "Yes");
}

TEST_CASE("ConditionParser: context.key access", "[condition]") {
    auto result = ConditionParser::parse("context.mode=debug");
    REQUIRE(result.ok());
    REQUIRE(result.value().clauses.size() == 1);
    REQUIRE(result.value().clauses[0].variable == "context.mode");
    REQUIRE(result.value().clauses[0].value == "debug");
}

TEST_CASE("ConditionParser: empty input", "[condition]") {
    auto result = ConditionParser::parse("");
    REQUIRE(result.ok());
    REQUIRE(result.value().empty());
}

TEST_CASE("ConditionParser: whitespace-only input", "[condition]") {
    auto result = ConditionParser::parse("   \t\n  ");
    REQUIRE(result.ok());
    REQUIRE(result.value().empty());
}

TEST_CASE("ConditionParser: malformed - missing operator", "[condition]") {
    auto result = ConditionParser::parse("outcome");
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("ConditionParser: malformed - missing value", "[condition]") {
    auto result = ConditionParser::parse("outcome=");
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("ConditionParser: quoted values", "[condition]") {
    auto result = ConditionParser::parse("outcome=\"success\"");
    REQUIRE(result.ok());
    REQUIRE(result.value().clauses[0].value == "success");
}

TEST_CASE("ConditionParser: spaces around operators", "[condition]") {
    auto result = ConditionParser::parse("outcome = success");
    REQUIRE(result.ok());
    REQUIRE(result.value().clauses[0].variable == "outcome");
    REQUIRE(result.value().clauses[0].op == "=");
    REQUIRE(result.value().clauses[0].value == "success");
}

TEST_CASE("ConditionParser: multiple conjunctions", "[condition]") {
    auto result = ConditionParser::parse("outcome=success && context.a=1 && context.b=2");
    REQUIRE(result.ok());
    REQUIRE(result.value().clauses.size() == 3);
}
