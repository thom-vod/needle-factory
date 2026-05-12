#include "needle/backend/prompt_assembly_policy.h"

namespace needle {

PromptAssemblyPolicyRegistry::PromptAssemblyPolicyRegistry() {
    default_policy_ = {false, false, WorkflowMode::Implementation, true, "implementer"};
    by_class_[""] = default_policy_;
    by_class_["coding"] = default_policy_;
    by_class_["implement"] = default_policy_;
    by_class_["fix"] = {false, false, WorkflowMode::Implementation, true, "implementer"};
    by_class_["apply_feedback"] = {false, true, WorkflowMode::Implementation, true, "implementer"};
    by_class_["critique"] = {true, false, WorkflowMode::NonCoding, false, "reviewer"};
    by_class_["review"] = {true, false, WorkflowMode::NonCoding, false, "reviewer"};
    by_class_["docs"] = {true, false, WorkflowMode::NonCoding, false, "documenter"};
    by_class_["troubleshoot"] = {true, false, WorkflowMode::NonCoding, false, "troubleshooter"};
}

const PromptAssemblyPolicy& PromptAssemblyPolicyRegistry::policy_for(const std::string& node_class) const {
    auto it = by_class_.find(node_class);
    if (it != by_class_.end()) {
        return it->second;
    }
    return default_policy_;
}

} // namespace needle
