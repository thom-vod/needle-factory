#include "needle/model/fidelity.h"
#include "needle/model/graph.h"

namespace needle {

FidelityMode fidelity_from_string(const std::string& s) {
    if (s == "truncate") return FidelityMode::TRUNCATE;
    if (s == "compact") return FidelityMode::COMPACT;
    if (s == "summary:low" || s == "summary_low") return FidelityMode::SUMMARY_LOW;
    if (s == "summary:medium" || s == "summary_medium") return FidelityMode::SUMMARY_MEDIUM;
    if (s == "summary:high" || s == "summary_high") return FidelityMode::SUMMARY_HIGH;
    if (s == "full") return FidelityMode::FULL;
    // Unknown string defaults to compact
    return FidelityMode::COMPACT;
}

std::string to_string(FidelityMode mode) {
    switch (mode) {
        case FidelityMode::TRUNCATE:       return "truncate";
        case FidelityMode::COMPACT:        return "compact";
        case FidelityMode::SUMMARY_LOW:    return "summary:low";
        case FidelityMode::SUMMARY_MEDIUM: return "summary:medium";
        case FidelityMode::SUMMARY_HIGH:   return "summary:high";
        case FidelityMode::FULL:           return "full";
    }
    return "compact";
}

FidelityMode resolve_fidelity(const Edge* edge, const Node& node, const Graph& graph) {
    // Edge-level override (highest precedence)
    if (edge) {
        std::string ef = edge->fidelity();
        if (!ef.empty()) {
            return fidelity_from_string(ef);
        }
    }

    // Node-level attribute
    std::string nf = node.attrs.get("fidelity");
    if (!nf.empty()) {
        return fidelity_from_string(nf);
    }

    // Graph-level default
    std::string gf = graph.graph_attrs().get("default_fidelity");
    if (!gf.empty()) {
        return fidelity_from_string(gf);
    }

    // Also check "fidelity" at graph level for backward compat
    gf = graph.graph_attrs().get("fidelity");
    if (!gf.empty()) {
        return fidelity_from_string(gf);
    }

    // Default to compact
    return FidelityMode::COMPACT;
}

} // namespace needle
