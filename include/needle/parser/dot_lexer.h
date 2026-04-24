#pragma once

#include <string>

namespace needle {

enum class TokenType {
    // Keywords
    DIGRAPH, GRAPH, SUBGRAPH, STRICT, NODE, EDGE,
    // Symbols
    LBRACE, RBRACE, LBRACKET, RBRACKET, SEMICOLON, COMMA,
    EQUALS, ARROW,  // ->
    // Literals
    IDENTIFIER, QUOTED_STRING, HTML_STRING, NUMBER,
    // Control
    END_OF_FILE, ERROR
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int column;
};

class DotLexer {
public:
    explicit DotLexer(const std::string& source);

    Token next_token();
    Token peek();
    bool at_end() const;

private:
    static const size_t MAX_IDENTIFIER_LENGTH = 1024;
    static const size_t MAX_STRING_LENGTH = 1048576;  // 1MB

    void skip_whitespace_and_comments();
    Token read_identifier_or_keyword();
    Token read_quoted_string();
    Token read_html_string();
    Token read_number();

    char current() const;
    char advance();
    bool has_more() const;

    std::string source_;
    size_t pos_;
    int line_;
    int col_;
    bool has_peeked_;
    Token peeked_;
};

} // namespace needle
