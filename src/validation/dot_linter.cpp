#include "needle/validation/dot_linter.h"

#include "needle/config/needle_config.h"
#include "needle/engine/variable_expansion_transform.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace needle {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::set<std::string> parse_suppress(const std::string& raw) {
    std::set<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (c == ',') {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.insert(cur);
    return out;
}

bool is_suppressed(const Graph& graph, const Node& node, const std::string& code) {
    auto graph_s = parse_suppress(graph.graph_attrs().get("lint_suppress"));
    if (graph_s.find(code) != graph_s.end()) return true;
    auto node_s = parse_suppress(node.attrs.get("lint_suppress"));
    return node_s.find(code) != node_s.end();
}

std::vector<std::string> extract_paths(const std::string& text) {
    std::vector<std::string> paths;
    static const std::regex file_pat(R"(([A-Za-z0-9_./\\:-]+\.(go|py|js|ts|rs|c|cpp|h|md|json|yaml|yml)))");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), file_pat);
         it != std::sregex_iterator(); ++it) {
        paths.push_back((*it)[1].matched ? it->str(0) : "");
    }
    return paths;
}

bool contains_imperative(const std::string& text) {
    std::string lower = to_lower(text);
    static const char* kWords[] = {"implement", "fix", "commit", "write the code", "change the function"};
    for (const char* w : kWords) {
        if (lower.find(w) != std::string::npos) return true;
    }
    return false;
}

std::string class_model_key(const std::string& cls) {
    if (cls == "critique" || cls == "review") return "defaults.critique_model";
    if (cls == "planning") return "defaults.planning_model";
    return "defaults.coding_model";
}

} // namespace

std::vector<LintWarning> DotLinter::lint(const Graph& graph, const std::map<std::string, std::string>& cli_vars) const {
    std::vector<LintWarning> out;

    Context ctx;
    for (const auto& kv : cli_vars) ctx.set("var." + kv.first, kv.second);
    Graph copy = graph;
    auto ve = make_typed_variable_expansion_transform();
    ve->apply(copy, ctx);
    std::set<std::string> declared_vars;
    std::string params = graph.graph_attrs().get("params");
    if (!params.empty()) {
        std::stringstream ss(params);
        std::string seg;
        while (std::getline(ss, seg, ',')) {
            auto c = seg.find(':');
            if (c != std::string::npos) {
                std::string name = seg.substr(0, c);
                name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char ch){return std::isspace(ch);}), name.end());
                if (!name.empty()) declared_vars.insert(name);
            }
        }
    }
    for (const auto& unresolved : ve->unresolved_vars()) {
        if (unresolved.second.size() > 4 && unresolved.second.substr(0, 4) == "var.") {
            const std::string key = unresolved.second.substr(4);
            if (declared_vars.find(key) != declared_vars.end()) continue;
            const Node* node = copy.find_node(unresolved.first);
            if (!node || is_suppressed(copy, *node, "W001")) continue;
            out.push_back({"W001", unresolved.first,
                "undeclared-var: $" + unresolved.second + " referenced but not declared/passed via --var", 0, "warning"});
        }
    }

    // W002
    for (const auto& node : copy.nodes()) {
        if (node.type != NodeType::PARALLEL || is_suppressed(copy, node, "W002")) continue;
        auto edges = copy.outgoing_edges(node.id);
        std::map<std::string, std::vector<std::string>> file_to_nodes;
        for (const auto* e : edges) {
            const Node* child = copy.find_node(e->to);
            if (!child) continue;
            std::string corpus = child->attrs.get("prompt") + "\n" + child->attrs.get("command");
            for (const auto& p : extract_paths(corpus)) {
                file_to_nodes[p].push_back(child->id);
            }
        }
        for (const auto& kv : file_to_nodes) {
            if (kv.second.size() > 1) {
                LintWarning w;
                w.code = "W002";
                w.node_id = node.id;
                w.message = "parallel-sibling-file-conflict on " + kv.first;
                out.push_back(std::move(w));
            }
        }
    }

    // W003/W004
    for (const auto& node : copy.nodes()) {
        std::string cls = node.attrs.get("class");
        if ((cls == "critique" || cls == "review" || cls == "docs") &&
            !is_suppressed(copy, node, "W003") &&
            contains_imperative(node.prompt())) {
            out.push_back({"W003", node.id,
                "role-imperative-contradiction in non-coding class prompt", 0, "warning"});
        }
        if (!is_suppressed(copy, node, "W004")) {
            std::string model = node.attrs.get("llm_model");
            if (!model.empty()) {
                std::string key = class_model_key(cls);
                std::string cfg = NeedleConfig::global().get_string(key);
                if (!cfg.empty() && cfg != model) {
                    out.push_back({"W004", node.id,
                        "hardcoded-model differs from " + key + "; prefer $context.config." + key.substr(9), 0, "warning"});
                }
            }
        }
    }

    return out;
}

} // namespace needle
