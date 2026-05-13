#include "needle/engine/resume_validator.h"

#include <algorithm>
#include <sstream>
#include <cstdint>
#include <cstdio>

namespace needle {

Diagnostics ResumeValidator::validate(const Checkpoint& cp,
                                      const Graph& graph,
                                      bool strict_hash_check) {
    Diagnostics diags;

    // 1. ERROR if current_node doesn't exist in graph
    if (!graph.find_node(cp.current_node)) {
        Diagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.code = "R001";
        d.message = "Checkpoint current_node '" + cp.current_node + "' not found in graph";
        d.node_id = cp.current_node;
        diags.add(std::move(d));
    }

    // 2. ERROR if any completed_node doesn't exist in graph
    for (const auto& node_id : cp.completed_nodes) {
        if (!graph.find_node(node_id)) {
            Diagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.code = "R002";
            d.message = "Checkpoint completed_node '" + node_id + "' not found in graph";
            d.node_id = node_id;
            diags.add(std::move(d));
        }
    }

    // 3. Soft graph-hash check (N3). Walks completed nodes' per-node hashes
    // when the overall hash differs:
    //   - If all completed nodes' per-node hashes still match: graph was
    //     edited but only on unstarted nodes. Warn quietly and continue.
    //   - If any completed node's hash differs: surface which node and at
    //     what severity. With `strict_hash_check`, that's an ERROR (blocks
    //     resume); without, a WARNING.
    if (!cp.graph_hash.empty()) {
        std::string current_hash = compute_graph_hash(graph);
        if (cp.graph_hash != current_hash) {
            std::vector<std::string> changed_completed;
            for (const auto& node_id : cp.completed_nodes) {
                const Node* n = graph.find_node(node_id);
                if (!n) continue;  // already raised R002
                auto it = cp.completed_node_hashes.find(node_id);
                if (it == cp.completed_node_hashes.end()) {
                    // No per-node hash recorded — older checkpoint format.
                    // Skip; the overall mismatch still surfaces below.
                    continue;
                }
                if (compute_node_hash(*n) != it->second) {
                    changed_completed.push_back(node_id);
                }
            }
            if (changed_completed.empty()) {
                Diagnostic d;
                d.severity = DiagnosticSeverity::Warning;
                d.code = "R003";
                d.message = "Graph edited since checkpoint; only unstarted nodes affected — "
                            "continuing.";
                d.node_id = "";
                diags.add(std::move(d));
            } else {
                Diagnostic d;
                d.severity = strict_hash_check
                                 ? DiagnosticSeverity::Error
                                 : DiagnosticSeverity::Warning;
                d.code = "R003";
                std::string list;
                for (size_t i = 0; i < changed_completed.size(); ++i) {
                    if (i) list += ", ";
                    list += changed_completed[i];
                }
                d.message = std::string(strict_hash_check ? "" : "soft-hash check: ") +
                            "completed node(s) edited since checkpoint: " + list +
                            ". " + (strict_hash_check
                                        ? "Resume blocked under --strict-graph-hash."
                                        : "Resume continues; pass --strict-graph-hash to block.");
                d.node_id = "";
                diags.add(std::move(d));
            }
        }
    }

    // 4. WARNING if edges from current_node have changed
    //    Only check if current_node exists (otherwise we already have an ERROR)
    const Node* current = graph.find_node(cp.current_node);
    if (current) {
        // We can't compare against saved edge info since checkpoints don't store edges,
        // but we can warn if there are no outgoing edges from current_node
        // (meaning the graph may have been restructured).
        auto outgoing = graph.outgoing_edges(cp.current_node);
        if (outgoing.empty()) {
            // current_node has no outgoing edges -- this is suspicious unless it's the exit node
            const Node* exit_node = graph.exit_node();
            if (!exit_node || exit_node->id != cp.current_node) {
                Diagnostic d;
                d.severity = DiagnosticSeverity::Warning;
                d.code = "R004";
                d.message = "Checkpoint current_node '" + cp.current_node +
                            "' has no outgoing edges in graph";
                d.node_id = cp.current_node;
                diags.add(std::move(d));
            }
        }
    }

    return diags;
}

std::string ResumeValidator::compute_graph_hash(const Graph& graph) {
    // Collect all node IDs sorted, with their handler type
    struct NodeInfo {
        std::string id;
        std::string handler;
    };
    std::vector<NodeInfo> node_infos;
    for (const auto& node : graph.nodes()) {
        NodeInfo ni;
        ni.id = node.id;
        ni.handler = node.handler_type();
        node_infos.push_back(std::move(ni));
    }
    std::sort(node_infos.begin(), node_infos.end(),
              [](const NodeInfo& a, const NodeInfo& b) { return a.id < b.id; });

    // Collect all edges sorted by from->to, including conditions
    struct EdgeInfo {
        std::string from;
        std::string to;
        std::string condition;
    };
    std::vector<EdgeInfo> edge_infos;
    for (const auto& edge : graph.edges()) {
        EdgeInfo ei;
        ei.from = edge.from;
        ei.to = edge.to;
        ei.condition = edge.condition();
        edge_infos.push_back(std::move(ei));
    }
    std::sort(edge_infos.begin(), edge_infos.end(),
              [](const EdgeInfo& a, const EdgeInfo& b) {
                  if (a.from != b.from) return a.from < b.from;
                  if (a.to != b.to) return a.to < b.to;
                  return a.condition < b.condition;
              });

    // Concatenate into a deterministic string
    std::string input;
    for (const auto& ni : node_infos) {
        input += "N:" + ni.id + ":" + ni.handler + ";";
    }
    for (const auto& ei : edge_infos) {
        input += "E:" + ei.from + "->" + ei.to + ":" + ei.condition + ";";
    }

    // FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    for (char c : input) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }

    // Convert to hex string
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

std::string ResumeValidator::compute_node_hash(const Node& node) {
    // Per-node hash for the soft-hash check. Includes id + handler + every
    // attribute, so prompt edits, command edits, timeout bumps, and fidelity
    // changes all flip the hash. Sort attribute keys so insertion order
    // doesn't influence the result.
    std::string input = "id:" + node.id + ";handler:" + node.handler_type() + ";";

    // node.attrs.raw() returns a sorted std::map, so iteration order is
    // already deterministic across runs.
    for (const auto& kv : node.attrs.raw()) {
        input += kv.first + "=" + kv.second + ";";
    }

    uint64_t hash = 14695981039346656037ULL;
    for (char c : input) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }

    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

} // namespace needle
