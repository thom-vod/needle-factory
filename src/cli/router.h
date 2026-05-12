#pragma once

#include <string>
#include <vector>
#include <map>
#include <atomic>

namespace needle {

struct CLIArgs {
    std::string command;            // "run", "resume", "validate", "serve", "config"
    std::vector<std::string> positionals;  // positional args after command
    std::string logs_dir;
    std::string stylesheet;
    std::string backend;            // "cli" or "llmkit"
    std::string interviewer_mode;   // "console", "auto", "queue"
    std::string fidelity;
    bool no_color;
    bool json_output;
    bool dry_run;
    bool debug;
    bool quiet;
    int port;
    std::string bind_addr;
    bool help;
    bool version;
    std::map<std::string, std::string> vars;  // --var key=value
    std::string project_dir;                    // --project-dir (default: cwd)
    std::string stage_output;                   // --output (for `stage mark`)
    std::string stage_to;                       // --to (for `stage advance`)
    bool strict_graph_hash = false;             // --strict-graph-hash (for resume)
    bool allow_unresolved_vars = false;         // --allow-unresolved-vars
    bool troubleshoot = false;                  // --troubleshoot
    bool no_lint = false;                       // --no-lint
    bool strict = false;                        // --strict
    std::string scope;                          // --scope

    CLIArgs()
        : no_color(false)
        , json_output(false)
        , dry_run(false)
        , debug(false)
        , quiet(false)
        , port(0)
        , bind_addr()
        , help(false)
        , version(false) {}

    // Backward-compatible helper: return first positional or empty string.
    std::string first_positional() const {
        return positionals.empty() ? "" : positionals[0];
    }
};

class Router {
public:
    explicit Router(std::atomic<bool>& cancelled);
    int dispatch(int argc, char* argv[]);

private:
    int run_command(const CLIArgs& args);
    int resume_command(const CLIArgs& args);
    int validate_command(const CLIArgs& args);
    int dot_lint_command(const CLIArgs& args);
    int dot_rules_command(const CLIArgs& args);
    int serve_command(const CLIArgs& args);
    int auth_command(const CLIArgs& args);
    int status_command(const CLIArgs& args);
    int config_command(const CLIArgs& args);
    int attach_command(const CLIArgs& args);
    int retry_command(const CLIArgs& args);
    int stage_command(const CLIArgs& args);
    int troubleshoot_command(const CLIArgs& args);
    void print_usage();
    void print_version();

    CLIArgs parse_args(int argc, char* argv[]);

    std::atomic<bool>& cancelled_;
};

} // namespace needle
