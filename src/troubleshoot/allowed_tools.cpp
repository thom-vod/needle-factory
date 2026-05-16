#include "needle/troubleshoot/allowed_tools.h"

#include <sstream>

namespace needle {

namespace {

std::string quote_if_needed(const std::string& path) {
    if (path.find_first_of(" \t\n\"'") == std::string::npos) return path;
    std::string out = "'";
    for (char c : path) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string write_tool(const std::string& path) {
    return "Write(" + quote_if_needed(path) + ")";
}

std::string edit_tool(const std::string& path) {
    return "Edit(" + quote_if_needed(path) + ")";
}

} // namespace

std::string build_allowed_tools(TroubleshootMode mode,
                                const std::string& project_dir,
                                const std::string& graph_path,
                                const std::string& recovery_dir) {
    if (mode == TroubleshootMode::Off) return "";
    if (mode == TroubleshootMode::Full) return "--dangerously-skip-permissions";

    std::ostringstream out;
    out << "Read Glob Grep "
        << write_tool(recovery_dir + "/recovery.md") << " "
        << write_tool(recovery_dir + "/agent.stdout.log") << " "
        << write_tool(recovery_dir + "/agent.stderr.log");

    if (mode == TroubleshootMode::Tweak) {
        const std::string graph_edit = graph_path.empty()
            ? project_dir + "/*.dot"
            : graph_path;
        out << " "
            << edit_tool(graph_edit) << " "
            << edit_tool(project_dir + "/.needle/**/stages/*/prompt.md") << " "
            << write_tool(recovery_dir + "/snapshot/*") << " "
            << "Bash(needle stage mark:*) "
            << "Bash(needle stage advance:*) "
            << "Bash(needle retry:*) "
            << "Bash(needle resume:*) "
            << "Bash(needle config set defaults.*:*) "
            << "Bash(git status:*) "
            << "Bash(git log:*) "
            << "Bash(git diff:*)";
    }

    return out.str();
}

} // namespace needle
