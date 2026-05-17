#include "needle/worktree/strategy.h"

#include <algorithm>
#include <cctype>

namespace needle {

WorktreeStrategy worktree_strategy_from_string(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "auto")   return WorktreeStrategy::Auto;
    if (lower == "manual") return WorktreeStrategy::Manual;
    return WorktreeStrategy::Off;
}

std::string to_string(WorktreeStrategy strategy) {
    switch (strategy) {
        case WorktreeStrategy::Auto:   return "auto";
        case WorktreeStrategy::Manual: return "manual";
        case WorktreeStrategy::Off:    return "off";
    }
    return "off";
}

Result<std::string> interpolate_template(const std::string& tmpl,
                                         const std::map<std::string, std::string>& params) {
    std::string out;
    out.reserve(tmpl.size());

    size_t i = 0;
    while (i < tmpl.size()) {
        if (i + 1 < tmpl.size() && tmpl[i] == '$' && tmpl[i + 1] == '{') {
            size_t end = tmpl.find('}', i + 2);
            if (end == std::string::npos) {
                return Result<std::string>::failure(
                    "unterminated `${` in template: " + tmpl);
            }
            std::string var = tmpl.substr(i + 2, end - i - 2);
            auto it = params.find(var);
            if (it == params.end()) {
                return Result<std::string>::failure(
                    "missing parameter `${" + var + "}` in template: " + tmpl);
            }
            out += it->second;
            i = end + 1;
        } else {
            out += tmpl[i];
            ++i;
        }
    }
    return Result<std::string>::success(std::move(out));
}

} // namespace needle
