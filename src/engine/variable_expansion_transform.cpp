#include "needle/engine/variable_expansion_transform.h"

#include <cctype>
#include <vector>
#include <utility>

namespace needle {

namespace {

// Check if character is part of an identifier (alphanumeric, underscore, dot)
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

bool is_terminal_punct(char c) {
    return c == '.' || c == ',' || c == ';' || c == ':' || c == ')' || c == ']' || c == '}';
}

// Expand {{key}} placeholders against needle.<key> context values.
// `{{logs_dir}}` resolves to the value of `needle.logs_dir` set by the
// run driver (CLI router, HTTP server). Unknown keys pass through unchanged
// so unrelated `{{...}}` patterns (e.g. in user-supplied template strings
// bound for downstream tools) are never mangled.
//
// Escape: `\{{...}}` passes through as literal `{{...}}` without expansion.
// Meta-pipelines that produce DOT files containing placeholder literals
// (e.g. a pipeline teaching an LLM to emit `{{logs_dir}}` into a
// generated DOT) write the DOT-string `\\{{logs_dir}}`, the lexer yields
// `\{{logs_dir}}`, and this pass emits `{{logs_dir}}` untouched.
std::string expand_placeholders(const std::string& input, const Context& ctx) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        if (i + 2 < input.size() && input[i] == '\\' &&
            input[i + 1] == '{' && input[i + 2] == '{') {
            out += "{{";
            i += 3;
            continue;
        }
        if (i + 1 < input.size() && input[i] == '{' && input[i + 1] == '{') {
            size_t close = input.find("}}", i + 2);
            if (close != std::string::npos) {
                std::string key = input.substr(i + 2, close - i - 2);
                size_t lo = 0;
                while (lo < key.size() && std::isspace(static_cast<unsigned char>(key[lo]))) ++lo;
                size_t hi = key.size();
                while (hi > lo && std::isspace(static_cast<unsigned char>(key[hi - 1]))) --hi;
                key = key.substr(lo, hi - lo);

                std::string full_key = "needle." + key;
                if (!key.empty() && ctx.has(full_key)) {
                    out += ctx.get(full_key);
                } else {
                    out.append(input, i, close - i + 2);
                }
                i = close + 2;
                continue;
            }
        }
        out += input[i];
        ++i;
    }
    return out;
}

// Extract a dotted identifier starting at pos (e.g., "var.seed", "context.parallel.consensus.result")
std::string extract_identifier(const std::string& input, size_t pos) {
    size_t start = pos;
    std::string id;
    while (pos < input.size() && is_ident_char(input[pos])) {
        id += input[pos];
        ++pos;
    }
    while (!id.empty() && is_terminal_punct(id.back())) {
        id.pop_back();
    }
    if (id.empty()) {
        return input.substr(start, pos - start);
    }
    return id;
}

// Resolve a variable name against context and graph
std::string resolve_variable(const std::string& var, const Context& ctx,
                             const Graph& graph, const Node& node) {
    // $goal -- graph-level goal attribute (recursively expand $var.* within it)
    if (var == "goal") {
        std::string goal = node.attrs.get("goal");
        if (goal.empty()) goal = graph.graph_attrs().get("goal");
        // Expand any $var.* references within the goal string itself
        if (goal.find('$') != std::string::npos) {
            std::string expanded;
            size_t j = 0;
            while (j < goal.size()) {
                if (goal[j] == '$' && j + 1 < goal.size() &&
                    (std::isalpha(static_cast<unsigned char>(goal[j + 1])) || goal[j + 1] == '_')) {
                    std::string inner = extract_identifier(goal, j + 1);
                    if (!inner.empty() && inner != "goal") { // Prevent infinite recursion
                        std::string resolved = resolve_variable(inner, ctx, graph, node);
                        if (!resolved.empty()) {
                            expanded += resolved;
                            j += 1 + inner.size();
                            continue;
                        }
                    }
                }
                expanded += goal[j];
                ++j;
            }
            goal = expanded;
        }
        return goal;
    }

    // $var.key -- from --var CLI flag (stored as var.key in context)
    if (var.size() > 4 && var.substr(0, 4) == "var.") {
        if (ctx.has(var)) return ctx.get(var);
        return ""; // --var not set
    }

    // $context.key -- explicit context reference (late-bound, resolved at runtime)
    if (var.size() > 8 && var.substr(0, 8) == "context.") {
        std::string key = var.substr(8);
        if (ctx.has(key)) return ctx.get(key);
        // Return empty WITHOUT flagging as unresolved — $context.* vars
        // are expected to be unavailable at graph-load time and will be
        // resolved when the node executes and prior stages have populated context.
        return "";
    }

    // Bare identifier -- try context first, then graph attrs
    if (ctx.has(var)) return ctx.get(var);
    std::string graph_val = graph.graph_attrs().get(var);
    if (!graph_val.empty()) return graph_val;

    return ""; // Unknown variable
}

// Variables that are expected to be unavailable at graph-load time.
// These are populated at runtime by prior stages and should not generate warnings.
bool is_late_bound(const std::string& var) {
    return (var.size() > 8 && var.substr(0, 8) == "context.") ||
           (var.size() > 6 && var.substr(0, 6) == "human.");
}

// Only $<known-prefix>.* (or the bare keyword $goal) is treated as a needle
// template reference. Bare $identifier that doesn't match a prefix is passed
// through verbatim — Svelte 5 runes ($state, $derived, $props, $effect),
// shell variables ($PATH), jQuery, regex backrefs ($1), etc. all look like
// variables syntactically but are never meant to be interpolated. Before
// this check, the expander warned on every one of them.
bool is_template_ref(const std::string& var) {
    return var == "goal" ||
           (var.size() > 4 && var.substr(0, 4) == "var.") ||
           (var.size() > 8 && var.substr(0, 8) == "context.") ||
           (var.size() > 6 && var.substr(0, 6) == "human.");
}

// Expand variable references in a string. If collector is non-null, record
// unresolved variable references as (node_id, variable_name) pairs.
std::string expand_variables(const std::string& input, const Context& ctx, const Graph& graph,
                             const Node& node,
                             std::vector<std::pair<std::string, std::string>>* collector) {
    std::string result;
    size_t i = 0;

    while (i < input.size()) {
        if (input[i] == '$') {
            // ${braced.form}
            if (i + 1 < input.size() && input[i + 1] == '{') {
                size_t close = input.find('}', i + 2);
                if (close != std::string::npos) {
                    std::string var = input.substr(i + 2, close - i - 2);
                    if (!is_template_ref(var)) {
                        // Not a needle template reference — pass through verbatim.
                        result += input.substr(i, close - i + 1);
                        i = close + 1;
                        continue;
                    }
                    std::string resolved = resolve_variable(var, ctx, graph, node);
                    if (!resolved.empty()) {
                        result += resolved;
                    } else {
                        // Keep unexpanded if unresolved
                        result += input.substr(i, close - i + 1);
                        if (collector && !is_late_bound(var)) {
                            collector->push_back(std::make_pair(node.id, var));
                        }
                    }
                    i = close + 1;
                    continue;
                }
            }

            // $dotted.identifier form
            if (i + 1 < input.size() && (std::isalpha(static_cast<unsigned char>(input[i + 1])) || input[i + 1] == '_')) {
                std::string var = extract_identifier(input, i + 1);
                if (!var.empty()) {
                    if (!is_template_ref(var)) {
                        // Literal $identifier (Svelte rune, shell var, etc.) —
                        // leave untouched, no warning.
                        result += '$';
                        result += var;
                        i += 1 + var.size();
                        continue;
                    }
                    std::string resolved = resolve_variable(var, ctx, graph, node);
                    if (!resolved.empty()) {
                        result += resolved;
                        i += 1 + var.size();
                        continue;
                    }
                    // Known prefix but unresolved — keep verbatim and flag
                    // unless late-bound.
                    result += '$';
                    result += var;
                    if (collector && !is_late_bound(var)) {
                        collector->push_back(std::make_pair(node.id, var));
                    }
                    i += 1 + var.size();
                    continue;
                }
            }

            result += input[i];
            ++i;
        } else {
            result += input[i];
            ++i;
        }
    }

    return result;
}

} // anonymous namespace

std::string VariableExpansionTransform::name() const {
    return "VariableExpansionTransform";
}

Result<void> VariableExpansionTransform::apply(Graph& graph, const Context& ctx) const {
    unresolved_.clear();

    // Attributes to expand in each node
    static const char* attrs_to_expand[] = {
        "prompt", "label", "query", "url", "dot_file",
        "command", "include_pattern",
        "provider", "mode", "fetch_type", "headed", "timeout",
        nullptr
    };

    for (auto& node : graph.mutable_nodes()) {
        for (int a = 0; attrs_to_expand[a]; ++a) {
            std::string val = node.attrs.get(attrs_to_expand[a]);
            if (!val.empty()) {
                // First pass: {{key}} placeholders (needle.<key> context values).
                // Second pass: $var.* / $context.* / $goal expansion.
                std::string placeholders_expanded = expand_placeholders(val, ctx);
                std::string expanded = expand_variables(placeholders_expanded, ctx, graph, node, &unresolved_);
                if (expanded != val) {
                    node.attrs.set(attrs_to_expand[a], expanded);
                }
            }
        }
    }

    return Result<void>::success();
}

std::vector<std::pair<std::string, std::string>> VariableExpansionTransform::unresolved_vars() const {
    return unresolved_;
}

std::shared_ptr<Transform> make_variable_expansion_transform() {
    return std::make_shared<VariableExpansionTransform>();
}

std::shared_ptr<VariableExpansionTransform> make_typed_variable_expansion_transform() {
    return std::make_shared<VariableExpansionTransform>();
}

} // namespace needle
