#pragma once

#include "needle/model/graph.h"
#include <vector>

namespace needle {
namespace fixtures {

inline Graph make_simple_graph() {
    std::vector<Node> nodes;
    {
        Node n; n.id = "start"; n.type = NodeType::START;
        n.attrs.set("shape", "Mdiamond");
        n.attrs.set("label", "Start");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "work"; n.type = NodeType::CODERGEN;
        n.attrs.set("shape", "box");
        n.attrs.set("label", "Do Work");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "end"; n.type = NodeType::EXIT;
        n.attrs.set("shape", "Msquare");
        n.attrs.set("label", "End");
        nodes.push_back(std::move(n));
    }

    std::vector<Edge> edges;
    {
        Edge e; e.from = "start"; e.to = "work";
        edges.push_back(std::move(e));
    }
    {
        Edge e; e.from = "work"; e.to = "end";
        edges.push_back(std::move(e));
    }

    return Graph::make("simple", std::move(nodes), std::move(edges));
}

inline Graph make_parallel_graph() {
    std::vector<Node> nodes;
    {
        Node n; n.id = "start"; n.type = NodeType::START;
        n.attrs.set("shape", "Mdiamond");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "fork"; n.type = NodeType::PARALLEL;
        n.attrs.set("shape", "component");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "branch_a"; n.type = NodeType::CODERGEN;
        n.attrs.set("shape", "box");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "branch_b"; n.type = NodeType::CODERGEN;
        n.attrs.set("shape", "box");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "join"; n.type = NodeType::FAN_IN;
        n.attrs.set("shape", "trapezium");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "end"; n.type = NodeType::EXIT;
        n.attrs.set("shape", "Msquare");
        nodes.push_back(std::move(n));
    }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "fork"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "fork"; e.to = "branch_a"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "fork"; e.to = "branch_b"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "branch_a"; e.to = "join"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "branch_b"; e.to = "join"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "join"; e.to = "end"; edges.push_back(std::move(e)); }

    return Graph::make("parallel", std::move(nodes), std::move(edges));
}

inline Graph make_conditional_graph() {
    std::vector<Node> nodes;
    {
        Node n; n.id = "start"; n.type = NodeType::START;
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "check"; n.type = NodeType::CONDITIONAL;
        n.attrs.set("shape", "diamond");
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "yes_path"; n.type = NodeType::CODERGEN;
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "no_path"; n.type = NodeType::CODERGEN;
        nodes.push_back(std::move(n));
    }
    {
        Node n; n.id = "end"; n.type = NodeType::EXIT;
        nodes.push_back(std::move(n));
    }

    std::vector<Edge> edges;
    { Edge e; e.from = "start"; e.to = "check"; edges.push_back(std::move(e)); }
    {
        Edge e; e.from = "check"; e.to = "yes_path";
        e.attrs.set("label", "Yes");
        e.attrs.set("condition", "outcome=success");
        edges.push_back(std::move(e));
    }
    {
        Edge e; e.from = "check"; e.to = "no_path";
        e.attrs.set("label", "No");
        e.attrs.set("condition", "outcome=failure");
        edges.push_back(std::move(e));
    }
    { Edge e; e.from = "yes_path"; e.to = "end"; edges.push_back(std::move(e)); }
    { Edge e; e.from = "no_path"; e.to = "end"; edges.push_back(std::move(e)); }

    return Graph::make("conditional", std::move(nodes), std::move(edges));
}

} // namespace fixtures
} // namespace needle
