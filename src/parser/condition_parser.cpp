#include "needle/parser/condition_parser.h"
#include <cctype>
#include <sstream>

namespace needle {

namespace {

struct ConditionLexer {
    const std::string& input;
    size_t pos;

    explicit ConditionLexer(const std::string& s) : input(s), pos(0) {}

    void skip_ws() {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
            pos++;
        }
    }

    bool at_end() const {
        return pos >= input.size();
    }

    char peek() const {
        if (pos < input.size()) return input[pos];
        return '\0';
    }

    std::string read_variable() {
        skip_ws();
        std::string result;
        while (pos < input.size()) {
            char c = input[pos];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
                result += c;
                pos++;
            } else {
                break;
            }
        }
        return result;
    }

    std::string read_operator() {
        skip_ws();
        if (pos < input.size() && input[pos] == '!') {
            if (pos + 1 < input.size() && input[pos + 1] == '=') {
                pos += 2;
                return "!=";
            }
        }
        if (pos < input.size() && input[pos] == '=') {
            pos++;
            return "=";
        }
        return "";
    }

    std::string read_value() {
        skip_ws();
        if (pos < input.size() && input[pos] == '"') {
            // Quoted value
            pos++; // skip opening "
            std::string result;
            while (pos < input.size() && input[pos] != '"') {
                if (input[pos] == '\\' && pos + 1 < input.size()) {
                    pos++;
                    result += input[pos];
                } else {
                    result += input[pos];
                }
                pos++;
            }
            if (pos < input.size()) {
                pos++; // skip closing "
            }
            return result;
        }
        // Unquoted value
        std::string result;
        while (pos < input.size()) {
            char c = input[pos];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-') {
                result += c;
                pos++;
            } else {
                break;
            }
        }
        return result;
    }

    bool match_and() {
        skip_ws();
        if (pos + 1 < input.size() && input[pos] == '&' && input[pos + 1] == '&') {
            pos += 2;
            return true;
        }
        return false;
    }
};

} // anonymous namespace

Result<ConditionExpr> ConditionParser::parse(const std::string& expr) {
    ConditionExpr result;

    // Check for empty / whitespace-only input
    bool all_ws = true;
    for (char c : expr) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            all_ws = false;
            break;
        }
    }
    if (expr.empty() || all_ws) {
        return Result<ConditionExpr>::success(std::move(result));
    }

    ConditionLexer lex(expr);

    while (true) {
        lex.skip_ws();
        if (lex.at_end()) break;

        std::string variable = lex.read_variable();
        if (variable.empty()) {
            std::ostringstream oss;
            oss << "expected variable name at position " << lex.pos;
            return Result<ConditionExpr>::failure(oss.str());
        }

        std::string op = lex.read_operator();
        if (op.empty()) {
            std::ostringstream oss;
            oss << "expected '=' or '!=' after '" << variable << "'";
            return Result<ConditionExpr>::failure(oss.str());
        }

        std::string value = lex.read_value();
        if (value.empty()) {
            std::ostringstream oss;
            oss << "expected value after '" << variable << op << "'";
            return Result<ConditionExpr>::failure(oss.str());
        }

        ConditionClause clause;
        clause.variable = variable;
        clause.op = op;
        clause.value = value;
        result.clauses.push_back(std::move(clause));

        lex.skip_ws();
        if (lex.at_end()) break;

        if (!lex.match_and()) {
            std::ostringstream oss;
            oss << "expected '&&' or end of input at position " << lex.pos;
            return Result<ConditionExpr>::failure(oss.str());
        }
    }

    return Result<ConditionExpr>::success(std::move(result));
}

} // namespace needle
