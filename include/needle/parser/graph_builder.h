#pragma once

#include <string>
#include <vector>
#include <map>
#include "needle/model/result.h"
#include "needle/model/graph.h"
#include "needle/parser/dot_ast.h"

namespace needle {

class GraphBuilder {
public:
    Result<Graph> build(const AstGraph& ast);

private:
    NodeType resolve_node_type(const AttributeMap& attrs);
    void apply_default_attrs(
        const std::vector<AstDefaultAttrs>& defaults,
        std::map<std::string, AttributeMap>& node_attrs,
        std::vector<Edge>& edges,
        AttributeMap& graph_attrs);
    void expand_edge_chains(
        const AstEdge& ast_edge,
        std::vector<Edge>& out);
    void expand_subgraphs(
        const AstSubgraph& sub,
        std::map<std::string, AttributeMap>& node_attrs,
        std::vector<Edge>& edges,
        const std::vector<AstDefaultAttrs>& parent_defaults);
    std::string sanitize_node_id(const std::string& id);
};

} // namespace needle
