#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include "needle/model/outcome.h"
#include "needle/model/context.h"

namespace needle {

namespace detail {
inline std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}
} // namespace detail

struct ConditionClause {
    std::string variable;   // "outcome" or "context.key" or "preferred_label"
    std::string op;         // "=" or "!="
    std::string value;      // the comparison value
};

struct ConditionExpr {
    std::vector<ConditionClause> clauses; // all joined by &&

    bool evaluate(const Outcome& outcome, const Context& ctx) const {
        for (const auto& clause : clauses) {
            std::string actual;
            if (clause.variable == "outcome") {
                actual = to_string(outcome.status);
            } else if (clause.variable == "preferred_label") {
                actual = outcome.preferred_label;
            } else if (clause.variable.substr(0, 8) == "context.") {
                std::string key = clause.variable.substr(8);
                actual = ctx.get(key);
            } else {
                // Unknown variable, treat as empty string
                actual = "";
            }

            bool match = (detail::to_lower(actual) == detail::to_lower(clause.value));
            if (clause.op == "!=") {
                match = !match;
            }
            if (!match) {
                return false;
            }
        }
        return true;
    }

    bool empty() const {
        return clauses.empty();
    }
};

} // namespace needle
