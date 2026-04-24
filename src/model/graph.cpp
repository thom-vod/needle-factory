#include "needle/model/graph.h"

namespace needle {

// Node accessors

std::string Node::label() const {
    return attrs.get("label", id);
}

std::string Node::prompt() const {
    return attrs.get("prompt");
}

bool Node::goal_gate() const {
    Maybe<bool> val = attrs.get_bool("goal_gate");
    return val.has_value() && *val;
}

std::string Node::handler_type() const {
    // Explicit handler attribute overrides type-based lookup
    std::string h = attrs.get("handler");
    if (!h.empty()) {
        return h;
    }
    switch (type) {
        case NodeType::START:        return "start";
        case NodeType::EXIT:         return "exit";
        case NodeType::CODERGEN:     return "codergen";
        case NodeType::CONDITIONAL:  return "conditional";
        case NodeType::PARALLEL:     return "parallel";
        case NodeType::FAN_IN:       return "fan_in";
        case NodeType::WAIT_HUMAN:   return "wait_human";
        case NodeType::TOOL:         return "tool";
        case NodeType::MANAGER_LOOP: return "manager_loop";
        case NodeType::LLMKIT:       return "llmkit";
    }
    return "unknown";
}

// Edge accessors

std::string Edge::label() const {
    return attrs.get("label");
}

std::string Edge::condition() const {
    return attrs.get("condition");
}

int Edge::weight() const {
    Maybe<int> w = attrs.get_int("weight");
    return w.has_value() ? *w : 0;
}

std::string Edge::fidelity() const {
    return attrs.get("fidelity");
}

// Graph

const std::string& Graph::name() const {
    return name_;
}

const std::vector<Node>& Graph::nodes() const {
    return nodes_;
}

const std::vector<Edge>& Graph::edges() const {
    return edges_;
}

const Node* Graph::find_node(const std::string& id) const {
    for (const auto& n : nodes_) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

Node* Graph::mutable_node(const std::string& id) {
    for (auto& n : nodes_) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

std::vector<Node>& Graph::mutable_nodes() {
    return nodes_;
}

const Node* Graph::start_node() const {
    for (const auto& n : nodes_) {
        if (n.type == NodeType::START) {
            return &n;
        }
    }
    return nullptr;
}

const Node* Graph::exit_node() const {
    for (const auto& n : nodes_) {
        if (n.type == NodeType::EXIT) {
            return &n;
        }
    }
    return nullptr;
}

std::vector<const Edge*> Graph::outgoing_edges(const std::string& node_id) const {
    std::vector<const Edge*> result;
    for (const auto& e : edges_) {
        if (e.from == node_id) {
            result.push_back(&e);
        }
    }
    return result;
}

std::vector<const Edge*> Graph::incoming_edges(const std::string& node_id) const {
    std::vector<const Edge*> result;
    for (const auto& e : edges_) {
        if (e.to == node_id) {
            result.push_back(&e);
        }
    }
    return result;
}

std::set<std::string> Graph::reachable_from(const std::string& node_id) const {
    std::set<std::string> visited;
    std::vector<std::string> queue;
    queue.push_back(node_id);
    while (!queue.empty()) {
        std::string current = queue.back();
        queue.pop_back();
        if (visited.count(current)) continue;
        visited.insert(current);
        for (const auto& e : edges_) {
            if (e.from == current && !visited.count(e.to)) {
                queue.push_back(e.to);
            }
        }
    }
    return visited;
}

const AttributeMap& Graph::graph_attrs() const {
    return graph_attrs_;
}

Graph Graph::make(const std::string& name,
                  std::vector<Node> nodes,
                  std::vector<Edge> edges,
                  AttributeMap graph_attrs) {
    Graph g;
    g.name_ = name;
    g.nodes_ = std::move(nodes);
    g.edges_ = std::move(edges);
    g.graph_attrs_ = std::move(graph_attrs);
    return g;
}

} // namespace needle
