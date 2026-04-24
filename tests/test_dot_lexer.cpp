#include <catch2/catch.hpp>
#include "needle/parser/dot_lexer.h"

using namespace needle;

TEST_CASE("Lexer: simple tokens", "[lexer]") {
    DotLexer lex("{ } [ ] ; , = ->");
    REQUIRE(lex.next_token().type == TokenType::LBRACE);
    REQUIRE(lex.next_token().type == TokenType::RBRACE);
    REQUIRE(lex.next_token().type == TokenType::LBRACKET);
    REQUIRE(lex.next_token().type == TokenType::RBRACKET);
    REQUIRE(lex.next_token().type == TokenType::SEMICOLON);
    REQUIRE(lex.next_token().type == TokenType::COMMA);
    REQUIRE(lex.next_token().type == TokenType::EQUALS);
    REQUIRE(lex.next_token().type == TokenType::ARROW);
    REQUIRE(lex.next_token().type == TokenType::END_OF_FILE);
}

TEST_CASE("Lexer: keywords are case-insensitive", "[lexer]") {
    DotLexer lex("digraph DIGRAPH DiGraph graph GRAPH subgraph STRICT node EDGE");

    REQUIRE(lex.next_token().type == TokenType::DIGRAPH);
    REQUIRE(lex.next_token().type == TokenType::DIGRAPH);
    REQUIRE(lex.next_token().type == TokenType::DIGRAPH);
    REQUIRE(lex.next_token().type == TokenType::GRAPH);
    REQUIRE(lex.next_token().type == TokenType::GRAPH);
    REQUIRE(lex.next_token().type == TokenType::SUBGRAPH);
    REQUIRE(lex.next_token().type == TokenType::STRICT);
    REQUIRE(lex.next_token().type == TokenType::NODE);
    REQUIRE(lex.next_token().type == TokenType::EDGE);
}

TEST_CASE("Lexer: identifiers", "[lexer]") {
    DotLexer lex("hello world_123 _foo");
    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::IDENTIFIER);
    REQUIRE(t1.value == "hello");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::IDENTIFIER);
    REQUIRE(t2.value == "world_123");

    Token t3 = lex.next_token();
    REQUIRE(t3.type == TokenType::IDENTIFIER);
    REQUIRE(t3.value == "_foo");
}

TEST_CASE("Lexer: quoted strings", "[lexer]") {
    DotLexer lex("\"hello world\" \"with \\\"escapes\\\"\" \"newline\\n\"");

    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::QUOTED_STRING);
    REQUIRE(t1.value == "hello world");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::QUOTED_STRING);
    REQUIRE(t2.value == "with \"escapes\"");

    Token t3 = lex.next_token();
    REQUIRE(t3.type == TokenType::QUOTED_STRING);
    REQUIRE(t3.value == "newline\n");
}

TEST_CASE("Lexer: HTML strings", "[lexer]") {
    DotLexer lex("<hello> <<b>bold</b>>");

    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::HTML_STRING);
    REQUIRE(t1.value == "hello");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::HTML_STRING);
    REQUIRE(t2.value == "<b>bold</b>");
}

TEST_CASE("Lexer: HTML strings with deep nesting", "[lexer]") {
    DotLexer lex("<<table><tr><td>cell</td></tr></table>>");

    Token t = lex.next_token();
    REQUIRE(t.type == TokenType::HTML_STRING);
    REQUIRE(t.value == "<table><tr><td>cell</td></tr></table>");
}

TEST_CASE("Lexer: numbers", "[lexer]") {
    DotLexer lex("42 3.14 -7");

    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::NUMBER);
    REQUIRE(t1.value == "42");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::NUMBER);
    REQUIRE(t2.value == "3.14");

    Token t3 = lex.next_token();
    REQUIRE(t3.type == TokenType::NUMBER);
    REQUIRE(t3.value == "-7");
}

TEST_CASE("Lexer: C-style comments", "[lexer]") {
    DotLexer lex("hello /* comment */ world");

    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::IDENTIFIER);
    REQUIRE(t1.value == "hello");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::IDENTIFIER);
    REQUIRE(t2.value == "world");
}

TEST_CASE("Lexer: C++-style comments", "[lexer]") {
    DotLexer lex("hello // comment\nworld");

    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::IDENTIFIER);
    REQUIRE(t1.value == "hello");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::IDENTIFIER);
    REQUIRE(t2.value == "world");
}

TEST_CASE("Lexer: hash comments", "[lexer]") {
    DotLexer lex("# comment\nhello");

    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::IDENTIFIER);
    REQUIRE(t1.value == "hello");
}

TEST_CASE("Lexer: unterminated string", "[lexer]") {
    DotLexer lex("\"unterminated");

    Token t = lex.next_token();
    REQUIRE(t.type == TokenType::ERROR);
    REQUIRE(t.value == "unterminated string");
}

TEST_CASE("Lexer: line and column tracking", "[lexer]") {
    DotLexer lex("hello\nworld");

    Token t1 = lex.next_token();
    REQUIRE(t1.line == 1);
    REQUIRE(t1.column == 1);

    Token t2 = lex.next_token();
    REQUIRE(t2.line == 2);
    REQUIRE(t2.column == 1);
}

TEST_CASE("Lexer: arrow token", "[lexer]") {
    DotLexer lex("A -> B");

    Token t1 = lex.next_token();
    REQUIRE(t1.type == TokenType::IDENTIFIER);
    REQUIRE(t1.value == "A");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::ARROW);
    REQUIRE(t2.value == "->");

    Token t3 = lex.next_token();
    REQUIRE(t3.type == TokenType::IDENTIFIER);
    REQUIRE(t3.value == "B");
}

TEST_CASE("Lexer: peek does not consume", "[lexer]") {
    DotLexer lex("hello world");

    Token p = lex.peek();
    REQUIRE(p.type == TokenType::IDENTIFIER);
    REQUIRE(p.value == "hello");

    Token t = lex.next_token();
    REQUIRE(t.type == TokenType::IDENTIFIER);
    REQUIRE(t.value == "hello");

    Token t2 = lex.next_token();
    REQUIRE(t2.type == TokenType::IDENTIFIER);
    REQUIRE(t2.value == "world");
}

TEST_CASE("Lexer: empty input", "[lexer]") {
    DotLexer lex("");
    REQUIRE(lex.next_token().type == TokenType::END_OF_FILE);
    REQUIRE(lex.at_end());
}
