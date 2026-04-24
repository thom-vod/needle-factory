#include "needle/engine/transform.h"
#include "needle/parser/stylesheet_parser.h"

namespace needle {

class StylesheetTransform : public Transform {
public:
    explicit StylesheetTransform(Stylesheet stylesheet)
        : stylesheet_(std::move(stylesheet)) {}

    std::string name() const override { return "StylesheetTransform"; }

    Result<void> apply(Graph& graph, const Context& /*ctx*/) const override {
        for (auto& node : graph.mutable_nodes()) {
            // Collect matching rules sorted by specificity (universal < class < id)
            // Lower specificity rules apply first, higher ones override
            std::vector<const StyleRule*> universal_rules;
            std::vector<const StyleRule*> class_rules;
            std::vector<const StyleRule*> id_rules;

            for (const auto& rule : stylesheet_.rules) {
                switch (rule.selector_type) {
                    case StyleRule::UNIVERSAL:
                        universal_rules.push_back(&rule);
                        break;
                    case StyleRule::CLASS: {
                        // Match by class attribute on node
                        // Selector is ".classname" - strip the dot
                        std::string class_name = rule.selector.substr(1);
                        std::string node_class = node.attrs.get("class");
                        if (node_class == class_name) {
                            class_rules.push_back(&rule);
                        }
                        break;
                    }
                    case StyleRule::ID: {
                        // Match by node ID
                        // Selector is "#nodeid" - strip the hash
                        std::string id_name = rule.selector.substr(1);
                        if (node.id == id_name) {
                            id_rules.push_back(&rule);
                        }
                        break;
                    }
                }
            }

            // Build merged properties: universal, then class, then id
            // Each layer overrides the previous
            AttributeMap merged;
            for (const auto* rule : universal_rules) {
                for (const auto& kv : rule->properties.raw()) {
                    merged.set(kv.first, kv.second);
                }
            }
            for (const auto* rule : class_rules) {
                for (const auto& kv : rule->properties.raw()) {
                    merged.set(kv.first, kv.second);
                }
            }
            for (const auto* rule : id_rules) {
                for (const auto& kv : rule->properties.raw()) {
                    merged.set(kv.first, kv.second);
                }
            }

            // Apply merged properties but node-level attrs override stylesheet
            for (const auto& kv : merged.raw()) {
                if (!node.attrs.has(kv.first)) {
                    node.attrs.set(kv.first, kv.second);
                }
            }
        }

        return Result<void>::success();
    }

private:
    Stylesheet stylesheet_;
};

// Factory function used by tests and pipeline engine
std::shared_ptr<Transform> make_stylesheet_transform(Stylesheet stylesheet) {
    return std::make_shared<StylesheetTransform>(std::move(stylesheet));
}

} // namespace needle
