#pragma once

#include <string>
#include <map>
#include "needle/model/graph.h"

namespace needle {

// Serialize a Graph to DOT format suitable for graphviz rendering.
std::string graph_to_dot(const Graph& graph);

// Invoke `dot -Tsvg` on the given DOT source. Returns SVG string.
// Returns empty string if dot is not available or invocation fails.
std::string dot_to_svg(const std::string& dot_source);

// Convenience: graph_to_dot() -> dot_to_svg() -> inject ndl-node-{id} classes.
std::string graph_to_svg(const Graph& graph);

// HTML-escape a string: replaces <, >, &, ", ' with entities.
std::string html_escape(const std::string& input);

} // namespace needle
