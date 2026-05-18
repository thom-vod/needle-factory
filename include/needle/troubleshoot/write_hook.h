#pragma once

#include <string>
#include <vector>

namespace needle {

// Defence-in-depth check: returns true iff the canonicalised abs_path
// resolves under project_dir OR under session_dir. Used to audit
// agent-issued Edit/Write tool calls post-run. Caller passes already-
// absolute paths.
bool file_write_allowed(const std::string& abs_path,
                        const std::string& project_dir,
                        const std::string& session_dir);

struct HookViolation {
    std::string tool;        // "Edit" or "Write"
    std::string file_path;
    std::string tool_use_id;
};

// Parse events.ndjson at events_ndjson_path. For every tool_use of
// kind Edit / Write, extract input.file_path and check
// file_write_allowed(...). Returns the list of violations found.
std::vector<HookViolation> audit_events_ndjson(const std::string& events_ndjson_path,
                                               const std::string& project_dir,
                                               const std::string& session_dir);

} // namespace needle
