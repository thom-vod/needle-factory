#include "needle/parser/graph_builder.h"
#include <algorithm>
#include <cctype>

namespace needle {

Result<Graph> GraphBuilder::build(const AstGraph& ast) {
    std::map<std::string, AttributeMap> node_attrs;
    std::vector<Edge> edges;
    AttributeMap graph_attrs;

    // First, process default attrs at graph level
    apply_default_attrs(ast.defaults, node_attrs, edges, graph_attrs);

    // Process nodes from the AST
    for (const auto& ast_node : ast.nodes) {
        std::string san_id = sanitize_node_id(ast_node.id);
        AttributeMap& attrs = node_attrs[san_id];
        for (const auto& attr : ast_node.attrs) {
            attrs.set(attr.key, attr.value);
        }
    }

    // Process edges from the AST
    for (const auto& ast_edge : ast.edges) {
        expand_edge_chains(ast_edge, edges);
        // Ensure all nodes referenced in edges exist in node_attrs
        for (const auto& nid : ast_edge.node_chain) {
            std::string san_id = sanitize_node_id(nid);
            if (node_attrs.find(san_id) == node_attrs.end()) {
                node_attrs[san_id] = AttributeMap();
            }
        }
    }

    // Process subgraphs
    for (const auto& sub : ast.subgraphs) {
        expand_subgraphs(sub, node_attrs, edges, ast.defaults);
    }

    // Build final node list
    std::vector<Node> nodes;
    for (const auto& pair : node_attrs) {
        Node node;
        node.id = pair.first;
        node.attrs = pair.second;
        node.type = resolve_node_type(node.attrs);
        nodes.push_back(std::move(node));
    }

    Graph graph = Graph::make(ast.id, std::move(nodes), std::move(edges), std::move(graph_attrs));
    return Result<Graph>::success(std::move(graph));
}

NodeType GraphBuilder::resolve_node_type(const AttributeMap& attrs) {
    // Check handler attribute first for LLMKIT
    std::string handler = attrs.get("handler");
    if (handler == "llmkit") {
        return NodeType::LLMKIT;
    }

    std::string shape = attrs.get("shape");

    // Normalize shape to lower for comparison
    std::string lower_shape = shape;
    std::transform(lower_shape.begin(), lower_shape.end(), lower_shape.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower_shape == "mdiamond")      return NodeType::START;
    if (lower_shape == "msquare")       return NodeType::EXIT;
    if (lower_shape == "diamond")       return NodeType::CONDITIONAL;
    if (lower_shape == "component")     return NodeType::PARALLEL;
    if (lower_shape == "trapezium")     return NodeType::FAN_IN;
    if (lower_shape == "hexagon")       return NodeType::WAIT_HUMAN;
    if (lower_shape == "parallelogram") return NodeType::TOOL;
    if (lower_shape == "house")         return NodeType::MANAGER_LOOP;
    if (lower_shape == "box")           return NodeType::CODERGEN;

    // Default to CODERGEN
    return NodeType::CODERGEN;
}

void GraphBuilder::apply_default_attrs(
    const std::vector<AstDefaultAttrs>& defaults,
    std::map<std::string, AttributeMap>& node_attrs,
    std::vector<Edge>& edges,
    AttributeMap& graph_attrs) {

    (void)node_attrs;
    (void)edges;

    for (const auto& def : defaults) {
        if (def.target == "graph") {
            for (const auto& attr : def.attrs) {
                graph_attrs.set(attr.key, attr.value);
            }
        }
        // node and edge defaults are applied later during node/edge construction
    }
}

void GraphBuilder::expand_edge_chains(
    const AstEdge& ast_edge,
    std::vector<Edge>& out) {

    for (size_t i = 0; i + 1 < ast_edge.node_chain.size(); ++i) {
        Edge edge;
        edge.from = sanitize_node_id(ast_edge.node_chain[i]);
        edge.to = sanitize_node_id(ast_edge.node_chain[i + 1]);
        for (const auto& attr : ast_edge.attrs) {
            edge.attrs.set(attr.key, attr.value);
        }
        out.push_back(std::move(edge));
    }
}

void GraphBuilder::expand_subgraphs(
    const AstSubgraph& sub,
    std::map<std::string, AttributeMap>& node_attrs,
    std::vector<Edge>& edges,
    const std::vector<AstDefaultAttrs>& parent_defaults) {

    // Collect node defaults from parent and subgraph
    std::vector<AstAttribute> node_default_attrs;
    for (const auto& def : parent_defaults) {
        if (def.target == "node") {
            node_default_attrs.insert(node_default_attrs.end(),
                                      def.attrs.begin(), def.attrs.end());
        }
    }
    for (const auto& def : sub.defaults) {
        if (def.target == "node") {
            node_default_attrs.insert(node_default_attrs.end(),
                                      def.attrs.begin(), def.attrs.end());
        }
    }

    // Process nodes in subgraph
    for (const auto& ast_node : sub.nodes) {
        std::string san_id = sanitize_node_id(ast_node.id);
        AttributeMap& attrs = node_attrs[san_id];
        // Apply defaults first
        for (const auto& attr : node_default_attrs) {
            if (!attrs.has(attr.key)) {
                attrs.set(attr.key, attr.value);
            }
        }
        // Then node-specific attrs (override)
        for (const auto& attr : ast_node.attrs) {
            attrs.set(attr.key, attr.value);
        }
    }

    // Process edges in subgraph
    for (const auto& ast_edge : sub.edges) {
        expand_edge_chains(ast_edge, edges);
        for (const auto& nid : ast_edge.node_chain) {
            std::string san_id = sanitize_node_id(nid);
            if (node_attrs.find(san_id) == node_attrs.end()) {
                AttributeMap& attrs = node_attrs[san_id];
                for (const auto& attr : node_default_attrs) {
                    attrs.set(attr.key, attr.value);
                }
            }
        }
    }

    // Process nested subgraphs
    std::vector<AstDefaultAttrs> combined_defaults = parent_defaults;
    combined_defaults.insert(combined_defaults.end(),
                             sub.defaults.begin(), sub.defaults.end());
    for (const auto& nested : sub.subgraphs) {
        expand_subgraphs(nested, node_attrs, edges, combined_defaults);
    }
}

std::string GraphBuilder::sanitize_node_id(const std::string& id) {
    std::string result;
    result.reserve(id.size());
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-') {
            result += c;
        }
    }
    return result;
}

} // namespace needle
