#include "needle/engine/edge_selector.h"
#include "needle/parser/condition_parser.h"
#include "needle/model/condition.h"
#include <algorithm>

namespace needle {

Result<const Edge*> EdgeSelector::select(
    const std::vector<const Edge*>& candidates,
    const Outcome& outcome,
    const Context& ctx) const
{
    if (candidates.empty()) {
        return Result<const Edge*>::failure("no candidate edges");
    }

    // Step 1: Filter by condition (no bypass for single-edge — M2 fix)
    std::vector<const Edge*> filtered = filter_by_condition(candidates, outcome, ctx);
    if (filtered.empty()) {
        return Result<const Edge*>::failure("no edges match conditions");
    }
    if (filtered.size() == 1) {
        return Result<const Edge*>::success(filtered[0]);
    }

    // Step 2: Preferred label match
    const Edge* result = match_preferred_label(filtered, outcome);
    if (result) {
        return Result<const Edge*>::success(result);
    }

    // Step 3: Suggested next match
    result = match_suggested_next(filtered, outcome);
    if (result) {
        return Result<const Edge*>::success(result);
    }

    // Step 4: Highest weight
    result = pick_highest_weight(filtered);
    if (result) {
        return Result<const Edge*>::success(result);
    }

    // Step 5: Lexical tiebreak
    result = pick_lexical(filtered);
    return Result<const Edge*>::success(result);
}

std::vector<const Edge*> EdgeSelector::filter_by_condition(
    const std::vector<const Edge*>& edges,
    const Outcome& outcome,
    const Context& ctx) const
{
    std::vector<const Edge*> unconditional;
    std::vector<const Edge*> matched;

    for (const auto* e : edges) {
        std::string cond_str = e->condition();
        if (cond_str.empty()) {
            unconditional.push_back(e);
            continue;
        }

        auto parse_result = ConditionParser::parse(cond_str);
        if (!parse_result.ok()) {
            // Unparseable condition: skip this edge
            continue;
        }

        const ConditionExpr& expr = parse_result.value();
        if (expr.empty()) {
            unconditional.push_back(e);
            continue;
        }

        if (expr.evaluate(outcome, ctx)) {
            matched.push_back(e);
        }
    }

    // If any conditional edges matched, prefer them over unconditional
    if (!matched.empty()) {
        return matched;
    }
    return unconditional;
}

const Edge* EdgeSelector::match_preferred_label(
    const std::vector<const Edge*>& edges,
    const Outcome& outcome) const
{
    if (outcome.preferred_label.empty()) {
        return nullptr;
    }
    for (const auto* e : edges) {
        if (e->label() == outcome.preferred_label) {
            return e;
        }
    }
    return nullptr;
}

const Edge* EdgeSelector::match_suggested_next(
    const std::vector<const Edge*>& edges,
    const Outcome& outcome) const
{
    for (const auto& next_id : outcome.suggested_next) {
        for (const auto* e : edges) {
            if (e->to == next_id) {
                return e;
            }
        }
    }
    return nullptr;
}

const Edge* EdgeSelector::pick_highest_weight(
    const std::vector<const Edge*>& edges) const
{
    if (edges.empty()) return nullptr;

    // Check if all weights are the same (all default 0)
    int max_weight = edges[0]->weight();
    bool all_same = true;
    for (size_t i = 1; i < edges.size(); ++i) {
        int w = edges[i]->weight();
        if (w != max_weight) {
            all_same = false;
        }
        if (w > max_weight) {
            max_weight = w;
        }
    }

    if (all_same) {
        return nullptr; // No differentiation by weight, fall through to lexical
    }

    // M4 fix: if multiple edges share max weight, fall through to lexical
    int tie_count = 0;
    const Edge* winner = nullptr;
    for (const auto* e : edges) {
        if (e->weight() == max_weight) {
            tie_count++;
            if (!winner) winner = e;
        }
    }
    if (tie_count > 1) {
        return nullptr; // Multiple max-weight edges — fall through to lexical tiebreak
    }
    return winner;
}

const Edge* EdgeSelector::pick_lexical(const std::vector<const Edge*>& edges) const {
    if (edges.empty()) return nullptr;

    const Edge* best = edges[0];
    for (size_t i = 1; i < edges.size(); ++i) {
        if (edges[i]->to < best->to) {
            best = edges[i];
        }
    }
    return best;
}

} // namespace needle
