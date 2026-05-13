#include "needle/validation/rules/params_attribute_syntax.h"

#include <cctype>
#include <vector>

namespace needle {

namespace {

std::string trim(const std::string& in) {
    size_t lo = 0;
    while (lo < in.size() && std::isspace(static_cast<unsigned char>(in[lo]))) ++lo;
    size_t hi = in.size();
    while (hi > lo && std::isspace(static_cast<unsigned char>(in[hi - 1]))) --hi;
    return in.substr(lo, hi - lo);
}

std::vector<std::string> split_segments(const std::string& params) {
    std::vector<std::string> out;
    std::string current;
    bool in_quotes = false;
    int paren_depth = 0;
    for (char c : params) {
        if (c == '"') in_quotes = !in_quotes;
        if (!in_quotes) {
            if (c == '(') ++paren_depth;
            else if (c == ')' && paren_depth > 0) --paren_depth;
            else if (c == ',' && paren_depth == 0) {
                out.push_back(trim(current));
                current.clear();
                continue;
            }
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(trim(current));
    return out;
}

} // namespace

void ParamsAttributeSyntaxRule::check(const Graph& graph, Diagnostics& diags) const {
    std::string params = graph.graph_attrs().get("params");
    if (params.empty()) return;
    for (const std::string& segment : split_segments(params)) {
        if (segment.empty()) continue;
        size_t eq = segment.find('=');
        size_t colon = segment.find(':');
        if (eq != std::string::npos && (colon == std::string::npos || eq < colon)) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.code = "E008";
            d.message = "Invalid params= segment '" + segment +
                        "': expected name:type[(options)]:default. "
                        "Use --var name=value for runtime values.";
            diags.add(std::move(d));
            return;
        }
    }
}

} // namespace needle
