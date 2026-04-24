#include "needle/parser/stylesheet_parser.h"
#include <cctype>
#include <sstream>

namespace needle {

namespace {

struct StyleLexer {
    const std::string& input;
    size_t pos;

    explicit StyleLexer(const std::string& s) : input(s), pos(0) {}

    void skip_ws() {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
            pos++;
        }
    }

    bool at_end() const {
        return pos >= input.size();
    }

    char peek() const {
        return pos < input.size() ? input[pos] : '\0';
    }

    char advance() {
        return pos < input.size() ? input[pos++] : '\0';
    }

    std::string read_identifier() {
        std::string result;
        while (pos < input.size()) {
            char c = input[pos];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
                result += c;
                pos++;
            } else {
                break;
            }
        }
        return result;
    }

    std::string read_value() {
        skip_ws();
        // Handle \" as opening quote (backslash-escaped quote from DOT embedding)
        bool backslash_quoted = false;
        if (pos + 1 < input.size() && input[pos] == '\\' && input[pos + 1] == '"') {
            backslash_quoted = true;
            pos += 2; // skip \"
        } else if (pos < input.size() && input[pos] == '"') {
            pos++; // skip "
        } else {
            // Unquoted value: read until ; or }
            std::string result;
            while (pos < input.size() && input[pos] != ';' && input[pos] != '}') {
                result += input[pos];
                pos++;
            }
            // Trim trailing whitespace
            while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
                result.pop_back();
            }
            return result;
        }
        // Quoted value (either " or \" delimited)
        std::string result;
        while (pos < input.size()) {
            if (backslash_quoted && pos + 1 < input.size() &&
                input[pos] == '\\' && input[pos + 1] == '"') {
                pos += 2; // closing \"
                return result;
            }
            if (!backslash_quoted && input[pos] == '"') {
                pos++; // closing "
                return result;
            }
            // Handle \\ escape inside quoted values
            if (input[pos] == '\\' && pos + 1 < input.size() &&
                input[pos + 1] != '"') {
                pos++; // skip backslash, take next char literally
            }
            result += input[pos];
            pos++;
        }
        return result;
    }
};

} // anonymous namespace

Result<Stylesheet> StylesheetParser::parse(const std::string& source) {
    Stylesheet stylesheet;
    StyleLexer lex(source);

    while (true) {
        lex.skip_ws();
        if (lex.at_end()) break;

        StyleRule rule;

        // Parse selector
        char c = lex.peek();
        if (c == '*') {
            rule.selector_type = StyleRule::UNIVERSAL;
            rule.selector = "*";
            lex.advance();
        } else if (c == '.') {
            lex.advance();
            std::string name = lex.read_identifier();
            if (name.empty()) {
                return Result<Stylesheet>::failure("expected class name after '.'");
            }
            rule.selector_type = StyleRule::CLASS;
            rule.selector = "." + name;
        } else if (c == '#') {
            lex.advance();
            std::string name = lex.read_identifier();
            if (name.empty()) {
                return Result<Stylesheet>::failure("expected ID name after '#'");
            }
            rule.selector_type = StyleRule::ID;
            rule.selector = "#" + name;
        } else {
            std::ostringstream oss;
            oss << "unexpected character '" << c << "' at position " << lex.pos;
            return Result<Stylesheet>::failure(oss.str());
        }

        lex.skip_ws();
        if (lex.at_end() || lex.peek() != '{') {
            return Result<Stylesheet>::failure("expected '{' after selector");
        }
        lex.advance(); // skip {

        // Parse properties
        while (true) {
            lex.skip_ws();
            if (lex.at_end()) {
                return Result<Stylesheet>::failure("unterminated rule block");
            }
            if (lex.peek() == '}') {
                lex.advance();
                break;
            }

            std::string key = lex.read_identifier();
            if (key.empty()) {
                return Result<Stylesheet>::failure("expected property name");
            }

            lex.skip_ws();
            if (lex.at_end() || (lex.peek() != ':' && lex.peek() != '=')) {
                return Result<Stylesheet>::failure("expected ':' or '=' after property name");
            }
            lex.advance(); // skip : or =

            std::string value = lex.read_value();

            rule.properties.set(key, value);

            // Optional semicolon
            lex.skip_ws();
            if (!lex.at_end() && lex.peek() == ';') {
                lex.advance();
            }
        }

        stylesheet.rules.push_back(std::move(rule));
    }

    return Result<Stylesheet>::success(std::move(stylesheet));
}

} // namespace needle
