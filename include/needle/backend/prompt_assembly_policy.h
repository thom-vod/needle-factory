#pragma once

#include <string>
#include <unordered_map>

namespace needle {

enum class WorkflowMode {
    Implementation,
    NonCoding,
    None,
};

struct PromptAssemblyPolicy {
    bool inject_role_isolation_preamble = false;
    bool auto_inject_human_feedback = false;
    WorkflowMode workflow_mode = WorkflowMode::Implementation;
    bool allow_skill_trailer = true;
    const char* role_label = "implementer";
};

class PromptAssemblyPolicyRegistry {
public:
    PromptAssemblyPolicyRegistry();
    const PromptAssemblyPolicy& policy_for(const std::string& node_class) const;

private:
    std::unordered_map<std::string, PromptAssemblyPolicy> by_class_;
    PromptAssemblyPolicy default_policy_;
};

} // namespace needle
