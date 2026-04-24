#pragma once

#include <string>
#include <map>
#include <memory>
#include <functional>
#include <vector>
#include "needle/backend/backend.h"
#include "needle/backend/process_runner.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"

namespace needle {

/// Result of wrapping a command: potentially rewritten command, args, and
/// environment overrides to apply on top of what the backend already set.
struct WrappedCommand {
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env_overrides;
};

/// Hook signature for rewriting a command before execution. External code
/// can install a wrapper to prepend a launcher (e.g. an attach/observe tool)
/// in front of the underlying CLI command. The wrapper receives the
/// already-resolved command+args and the current node/context, and returns
/// the final command to run.
using CommandWrapper = std::function<WrappedCommand(
    const std::string& command,
    const std::vector<std::string>& args,
    const Node& node,
    const Context& ctx)>;

struct CLITemplate {
    std::string command_pattern;   // e.g. "claude -p --model {model}"
    std::string provider;          // "claude", "codex", "gemini"
    std::map<std::string, std::string> defaults;  // default substitution values
    bool pipe_prompt_via_stdin = false;  // if true, pipe prompt to stdin instead of as arg
    int default_timeout_ms = 2700000;   // 45 minutes default
    bool supports_resume = false;       // if true, supports --resume <session_id>

    static CLITemplate claude_default();
    static CLITemplate codex_default();
    static CLITemplate gemini_default();
};

class CLIBackend : public Backend {
public:
    // Single-template constructor (backward compatible)
    explicit CLIBackend(CLITemplate tmpl, std::shared_ptr<ProcessRunner> runner);

    // Multi-template constructor: takes a default template and additional
    // templates keyed by provider name. Node attribute "llm_provider"
    // selects which template to use; falls back to the default.
    CLIBackend(CLITemplate default_tmpl,
               std::map<std::string, CLITemplate> templates,
               std::shared_ptr<ProcessRunner> runner);

    std::string name() const override;
    Result<Outcome> execute(const Node& node, Context& ctx,
                            const std::string& stage_dir) override;

    /// Install a command wrapper. If set, the wrapper is invoked just before
    /// each command is handed to the process runner, allowing external code
    /// to rewrite the command/args and contribute environment overrides.
    /// Pass an empty std::function to clear.
    void set_command_wrapper(CommandWrapper wrapper) { wrapper_ = std::move(wrapper); }

private:
    const CLITemplate& resolve_template(const Node& node) const;
    std::string build_command(const CLITemplate& tmpl, const Node& node,
                              const Context& ctx, const std::string& stage_dir);
    std::vector<std::string> build_args(const CLITemplate& tmpl, const Node& node,
                                        const Context& ctx, const std::string& stage_dir);

    CLITemplate default_template_;
    std::map<std::string, CLITemplate> templates_;
    std::shared_ptr<ProcessRunner> runner_;
    CommandWrapper wrapper_;
};

} // namespace needle
