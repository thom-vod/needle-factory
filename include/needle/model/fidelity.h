#pragma once

#include <string>

namespace needle {

enum class FidelityMode {
    TRUNCATE,
    COMPACT,
    SUMMARY_LOW,
    SUMMARY_MEDIUM,
    SUMMARY_HIGH,
    FULL
};

FidelityMode fidelity_from_string(const std::string& s);
std::string to_string(FidelityMode mode);

// Forward declarations
struct Edge;
struct Node;
class Graph;

// Resolution precedence: edge attribute > node attribute > graph default > COMPACT
FidelityMode resolve_fidelity(const Edge* edge, const Node& node, const Graph& graph);

} // namespace needle
