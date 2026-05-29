#pragma once

#include <string>
#include <map>
#include "needle/model/graph.h"

namespace needle {

// Serialize a Graph to DOT format suitable for graphviz rendering.
std::string graph_to_dot(const Graph& graph);

// Display-only DOT transform: give interactive (chat) nodes a distinct
// Graphviz shape (parallelogram) so operators can tell "the pipeline wants me
// to chat here" apart from autonomous nodes in the graph view. wait_human
// gates already use shape=hexagon by convention; interactive nodes otherwise
// fall back to the default box. Conservative — only single-line node
// statements that declare an interactive handler/type and have no explicit
// shape are touched; edges, default (graph/node/edge) statements, and
// malformed mid-edit DOT are left untouched. Used for rendering only, never
// for execution.
std::string normalize_display_shapes(const std::string& dot_source);

// Invoke `dot -Tsvg` on the given DOT source. Returns SVG string.
// Returns empty string if dot is not available or invocation fails.
std::string dot_to_svg(const std::string& dot_source);

// Convenience: graph_to_dot() -> dot_to_svg() -> inject ndl-node-{id} classes.
std::string graph_to_svg(const Graph& graph);

// HTML-escape a string: replaces <, >, &, ", ' with entities.
std::string html_escape(const std::string& input);

} // namespace needle
