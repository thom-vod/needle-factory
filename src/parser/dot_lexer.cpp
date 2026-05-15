#include "needle/parser/dot_lexer.h"
#include <algorithm>
#include <cctype>

namespace needle {

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

DotLexer::DotLexer(const std::string& source)
    : source_(source), pos_(0), line_(1), col_(1),
      has_peeked_(false), peeked_() {}

char DotLexer::current() const {
    if (pos_ < source_.size()) {
        return source_[pos_];
    }
    return '\0';
}

char DotLexer::advance() {
    char c = current();
    if (c == '\n') {
        line_++;
        col_ = 1;
    } else {
        col_++;
    }
    pos_++;
    return c;
}

bool DotLexer::has_more() const {
    return pos_ < source_.size();
}

bool DotLexer::at_end() const {
    // Need a non-const temporary to check
    DotLexer* self = const_cast<DotLexer*>(this);
    Token p = self->peek();
    return p.type == TokenType::END_OF_FILE;
}

Token DotLexer::peek() {
    if (!has_peeked_) {
        peeked_ = next_token();
        has_peeked_ = true;
    }
    return peeked_;
}

Token DotLexer::next_token() {
    if (has_peeked_) {
        has_peeked_ = false;
        return peeked_;
    }

    skip_whitespace_and_comments();

    if (!has_more()) {
        return Token{TokenType::END_OF_FILE, "", line_, col_};
    }

    char c = current();
    int start_line = line_;
    int start_col = col_;

    // Single-character tokens
    switch (c) {
        case '{': advance(); return Token{TokenType::LBRACE, "{", start_line, start_col};
        case '}': advance(); return Token{TokenType::RBRACE, "}", start_line, start_col};
        case '[': advance(); return Token{TokenType::LBRACKET, "[", start_line, start_col};
        case ']': advance(); return Token{TokenType::RBRACKET, "]", start_line, start_col};
        case ';': advance(); return Token{TokenType::SEMICOLON, ";", start_line, start_col};
        case ',': advance(); return Token{TokenType::COMMA, ",", start_line, start_col};
        case '=': advance(); return Token{TokenType::EQUALS, "=", start_line, start_col};
        default: break;
    }

    // Arrow: ->
    if (c == '-' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '>') {
        advance();
        advance();
        return Token{TokenType::ARROW, "->", start_line, start_col};
    }

    // Quoted string. Needle accepts single quotes here as a pragmatic
    // authoring convenience for dashboard/generated DOTs.
    if (c == '"' || c == '\'') {
        return read_quoted_string();
    }

    // HTML string
    if (c == '<') {
        return read_html_string();
    }

    // Number (including negative)
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && pos_ + 1 < source_.size() &&
         std::isdigit(static_cast<unsigned char>(source_[pos_ + 1])))) {
        return read_number();
    }

    // Dot after digit is handled in number
    if (c == '.' && pos_ + 1 < source_.size() &&
        std::isdigit(static_cast<unsigned char>(source_[pos_ + 1]))) {
        return read_number();
    }

    // Identifier or keyword
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return read_identifier_or_keyword();
    }

    // Unknown character
    advance();
    return Token{TokenType::ERROR, std::string(1, c), start_line, start_col};
}

void DotLexer::skip_whitespace_and_comments() {
    while (has_more()) {
        char c = current();

        // Whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        // C++-style comment: //
        if (c == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
            advance(); advance();
            while (has_more() && current() != '\n') {
                advance();
            }
            continue;
        }

        // C-style comment: /* */
        if (c == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            advance(); advance();
            while (has_more()) {
                if (current() == '*' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
                    advance(); advance();
                    break;
                }
                advance();
            }
            continue;
        }

        // # prefix comment (line comment)
        if (c == '#') {
            while (has_more() && current() != '\n') {
                advance();
            }
            continue;
        }

        break;
    }
}

Token DotLexer::read_identifier_or_keyword() {
    int start_line = line_;
    int start_col = col_;
    std::string value;

    while (has_more()) {
        char c = current();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            if (value.size() >= MAX_IDENTIFIER_LENGTH) {
                return Token{TokenType::ERROR, "identifier too long", start_line, start_col};
            }
            value += c;
            advance();
        } else {
            break;
        }
    }

    // Check for keywords (case-insensitive)
    std::string lower = to_lower(value);
    if (lower == "digraph") return Token{TokenType::DIGRAPH, value, start_line, start_col};
    if (lower == "graph")   return Token{TokenType::GRAPH, value, start_line, start_col};
    if (lower == "subgraph") return Token{TokenType::SUBGRAPH, value, start_line, start_col};
    if (lower == "strict")  return Token{TokenType::STRICT, value, start_line, start_col};
    if (lower == "node")    return Token{TokenType::NODE, value, start_line, start_col};
    if (lower == "edge")    return Token{TokenType::EDGE, value, start_line, start_col};

    return Token{TokenType::IDENTIFIER, value, start_line, start_col};
}

Token DotLexer::read_quoted_string() {
    int start_line = line_;
    int start_col = col_;
    char quote = current();
    advance(); // skip opening quote
    std::string value;

    while (has_more()) {
        char c = current();
        if (c == '\\') {
            advance();
            if (!has_more()) {
                return Token{TokenType::ERROR, "unterminated string", start_line, start_col};
            }
            char escaped = current();
            advance();
            switch (escaped) {
                case '"':  value += '"'; break;
                case '\'': value += '\''; break;
                case '\\': value += '\\'; break;
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                default:   value += '\\'; value += escaped; break;
            }
            if (value.size() > MAX_STRING_LENGTH) {
                return Token{TokenType::ERROR, "string too long", start_line, start_col};
            }
        } else if (c == quote) {
            advance(); // skip closing quote
            return Token{TokenType::QUOTED_STRING, value, start_line, start_col};
        } else {
            value += c;
            advance();
            if (value.size() > MAX_STRING_LENGTH) {
                return Token{TokenType::ERROR, "string too long", start_line, start_col};
            }
        }
    }

    return Token{TokenType::ERROR, "unterminated string", start_line, start_col};
}

Token DotLexer::read_html_string() {
    int start_line = line_;
    int start_col = col_;
    advance(); // skip opening <
    std::string value;
    int depth = 1;

    while (has_more() && depth > 0) {
        char c = current();
        if (c == '<') {
            depth++;
            value += c;
            advance();
        } else if (c == '>') {
            depth--;
            if (depth > 0) {
                value += c;
            }
            advance();
        } else {
            value += c;
            advance();
        }
        if (value.size() > MAX_STRING_LENGTH) {
            return Token{TokenType::ERROR, "HTML string too long", start_line, start_col};
        }
    }

    if (depth != 0) {
        return Token{TokenType::ERROR, "unterminated HTML string", start_line, start_col};
    }

    return Token{TokenType::HTML_STRING, value, start_line, start_col};
}

Token DotLexer::read_number() {
    int start_line = line_;
    int start_col = col_;
    std::string value;

    if (current() == '-') {
        value += current();
        advance();
    }

    while (has_more() && std::isdigit(static_cast<unsigned char>(current()))) {
        value += current();
        advance();
    }

    if (has_more() && current() == '.') {
        value += current();
        advance();
        while (has_more() && std::isdigit(static_cast<unsigned char>(current()))) {
            value += current();
            advance();
        }
    }

    return Token{TokenType::NUMBER, value, start_line, start_col};
}

} // namespace needle
