#pragma once

#include <string>
#include <vector>
#include <set>
#include "needle/model/attribute_map.h"

namespace needle {

enum class NodeType {
    START,
    EXIT,
    CODERGEN,
    CONDITIONAL,
    PARALLEL,
    FAN_IN,
    WAIT_HUMAN,
    TOOL,
    MANAGER_LOOP,
    LLMKIT
};

struct Node {
    std::string id;
    NodeType type;
    AttributeMap attrs;

    std::string label() const;
    std::string prompt() const;
    bool goal_gate() const;
    std::string handler_type() const;
};

struct Edge {
    std::string from;
    std::string to;
    AttributeMap attrs;

    std::string label() const;
    std::string condition() const;
    int weight() const;
    std::string fidelity() const;
};

class GraphBuilder;
template <typename T> class Result;

class Graph {
public:
    const std::string& name() const;
    const std::vector<Node>& nodes() const;
    const std::vector<Edge>& edges() const;

    const Node* find_node(const std::string& id) const;
    Node* mutable_node(const std::string& id);
    const Node* start_node() const;
    const Node* exit_node() const;
    std::vector<const Edge*> outgoing_edges(const std::string& node_id) const;
    std::vector<const Edge*> incoming_edges(const std::string& node_id) const;

    /// Return the set of all node IDs reachable from node_id via outgoing edges (BFS).
    std::set<std::string> reachable_from(const std::string& node_id) const;

    const AttributeMap& graph_attrs() const;
    std::vector<Node>& mutable_nodes();

    // Factory for testing (GraphBuilder is the normal construction path)
    static Graph make(const std::string& name,
                      std::vector<Node> nodes,
                      std::vector<Edge> edges,
                      AttributeMap graph_attrs = AttributeMap());

private:
    Graph() {}

    std::string name_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    AttributeMap graph_attrs_;

    friend class GraphBuilder;
    friend class Result<Graph>;
};

} // namespace needle
