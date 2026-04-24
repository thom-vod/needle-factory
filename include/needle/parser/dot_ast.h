#pragma once

#include <string>
#include <vector>

namespace needle {

struct AstAttribute {
    std::string key;
    std::string value;
};

struct AstNode {
    std::string id;
    std::vector<AstAttribute> attrs;
};

struct AstEdge {
    std::vector<std::string> node_chain;  // A -> B -> C = [A, B, C]
    std::vector<AstAttribute> attrs;      // applied to all edges in chain
};

struct AstDefaultAttrs {
    std::string target;  // "node", "edge", or "graph"
    std::vector<AstAttribute> attrs;
};

struct AstSubgraph {
    std::string id;
    std::vector<AstNode> nodes;
    std::vector<AstEdge> edges;
    std::vector<AstDefaultAttrs> defaults;
    std::vector<AstSubgraph> subgraphs;
};

struct AstGraph {
    bool strict;
    std::string id;
    std::vector<AstNode> nodes;
    std::vector<AstEdge> edges;
    std::vector<AstDefaultAttrs> defaults;
    std::vector<AstSubgraph> subgraphs;
};

} // namespace needle
