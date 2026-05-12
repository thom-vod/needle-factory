#include "needle/validation/graph_validator.h"
#include "needle/validation/rules/single_start_node.h"
#include "needle/validation/rules/single_exit_node.h"
#include "needle/validation/rules/start_no_incoming.h"
#include "needle/validation/rules/exit_no_outgoing.h"
#include "needle/validation/rules/all_nodes_reachable.h"
#include "needle/validation/rules/valid_conditions.h"
#include "needle/validation/rules/parallel_fan_in_pairing.h"
#include "needle/validation/rules/no_self_loops.h"
#include "needle/validation/rules/params_attribute_syntax.h"

namespace needle {

GraphValidator::GraphValidator(std::vector<std::shared_ptr<LintRule>> rules)
    : rules_(std::move(rules)) {}

Diagnostics GraphValidator::validate(const Graph& graph) const {
    Diagnostics diags;
    for (const auto& rule : rules_) {
        rule->check(graph, diags);
    }
    return diags;
}

GraphValidator GraphValidator::create_default() {
    std::vector<std::shared_ptr<LintRule>> rules;
    rules.push_back(std::make_shared<SingleStartNodeRule>());
    rules.push_back(std::make_shared<SingleExitNodeRule>());
    rules.push_back(std::make_shared<StartNoIncomingRule>());
    rules.push_back(std::make_shared<ExitNoOutgoingRule>());
    rules.push_back(std::make_shared<AllNodesReachableRule>());
    rules.push_back(std::make_shared<ValidConditionsRule>());
    rules.push_back(std::make_shared<ParallelFanInPairingRule>());
    rules.push_back(std::make_shared<NoSelfLoopsRule>());
    rules.push_back(std::make_shared<ParamsAttributeSyntaxRule>());
    return GraphValidator(std::move(rules));
}

} // namespace needle
