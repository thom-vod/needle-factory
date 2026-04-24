#pragma once

#include <vector>
#include "needle/model/result.h"
#include "needle/model/graph.h"
#include "needle/model/outcome.h"
#include "needle/model/context.h"

namespace needle {

class EdgeSelector {
public:
    Result<const Edge*> select(
        const std::vector<const Edge*>& candidates,
        const Outcome& outcome,
        const Context& ctx
    ) const;

private:
    std::vector<const Edge*> filter_by_condition(
        const std::vector<const Edge*>& edges, const Outcome& outcome, const Context& ctx) const;
    const Edge* match_preferred_label(
        const std::vector<const Edge*>& edges, const Outcome& outcome) const;
    const Edge* match_suggested_next(
        const std::vector<const Edge*>& edges, const Outcome& outcome) const;
    const Edge* pick_highest_weight(const std::vector<const Edge*>& edges) const;
    const Edge* pick_lexical(const std::vector<const Edge*>& edges) const;
};

} // namespace needle
