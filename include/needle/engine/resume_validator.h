#pragma once

#include <string>
#include <vector>
#include "needle/model/graph.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/validation/diagnostic.h"

namespace needle {

class ResumeValidator {
public:
    // Validate checkpoint against graph. Errors block resume, warnings proceed.
    static Diagnostics validate(const Checkpoint& cp, const Graph& graph);

    // Compute structural+semantic hash: sorted node IDs + handler types + edge pairs + edge conditions.
    static std::string compute_graph_hash(const Graph& graph);
};

} // namespace needle
