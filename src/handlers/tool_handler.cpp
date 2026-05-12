#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/backend/process_runner.h"
#include "needle/util/git_state.h"
#include <memory>

namespace needle {

class ToolHandler : public HandlerBase {
public:
    explicit ToolHandler(std::shared_ptr<ProcessRunner> runner)
        : runner_(std::move(runner)) {}

    std::string type_name() const override { return "tool"; }

    Result<Outcome> do_execute(const Node& node, Context& ctx,
                               const ExecutionContext& exec_ctx) override {
        std::string command = node.attrs.get("command");
        if (command.empty()) {
            return Result<Outcome>::failure("tool node missing 'command' attribute: " + node.id);
        }

        std::string cmd;
        std::vector<std::string> args;

        // Default to project directory so relative paths in commands resolve correctly
        std::string default_dir = exec_ctx.project_dir.empty() ? "." : exec_ctx.project_dir;
        if (node.attrs.get("cwd_scope") != "parent" && ctx.has("needle.branch.cwd")) {
            default_dir = ctx.get("needle.branch.cwd");
        }
        std::string working_dir = node.attrs.get("working_dir", default_dir);

#ifdef _WIN32
        // On Windows, always run through cmd.exe so .bat/.cmd files,
        // shell builtins, pipes, redirects, etc. all work correctly.
        // Strip Unix-style ./ prefix (cmd.exe doesn't understand it)
        std::string win_command = command;
        if (win_command.substr(0, 2) == "./") {
            win_command = win_command.substr(2);
        }
        // Also replace any remaining ./ with .\ for cmd.exe
        {
            size_t pos = 0;
            while ((pos = win_command.find("./", pos)) != std::string::npos) {
                win_command.replace(pos, 2, ".\\");
                pos += 2;
            }
        }
        // Use cd /D to ensure working directory is set within cmd.exe
        std::string win_dir = working_dir;
        for (auto& c : win_dir) { if (c == '/') c = '\\'; }
        cmd = "cmd.exe";
        args = {"/C", "cd /D " + win_dir + " && " + win_command};
#else
        // Detect if command uses shell constructs (pipes, redirects, &&, ||, etc.)
        bool needs_shell = (command.find('|') != std::string::npos ||
                            command.find("&&") != std::string::npos ||
                            command.find("||") != std::string::npos ||
                            command.find('>') != std::string::npos ||
                            command.find('<') != std::string::npos ||
                            command.find('`') != std::string::npos ||
                            command.find('$') != std::string::npos ||
                            command.find(';') != std::string::npos);

        if (needs_shell) {
            cmd = "/bin/sh";
            args = {"-c", command};
        } else {
            // Simple command: split by spaces
            std::vector<std::string> parts;
            std::string current;
            for (size_t i = 0; i < command.size(); ++i) {
                if (command[i] == ' ' || command[i] == '\t') {
                    if (!current.empty()) {
                        parts.push_back(current);
                        current.clear();
                    }
                } else {
                    current += command[i];
                }
            }
            if (!current.empty()) {
                parts.push_back(current);
            }

            if (parts.empty()) {
                return Result<Outcome>::failure("empty command for tool node: " + node.id);
            }

            cmd = parts[0];
            args.assign(parts.begin() + 1, parts.end());
        }
#endif

        int timeout_ms = 60000; // 1 minute default
        Maybe<int> t = node.attrs.get_duration_ms("timeout");
        if (t.has_value()) {
            timeout_ms = *t;
        }

        // Idle-output timeout: tool nodes default to disabled (0) since some
        // legitimate tool commands (long builds, archives, downloads) produce
        // no output for stretches. Authors opt in per-node when meaningful.
        int idle_timeout_ms = 0;
        Maybe<int> it = node.attrs.get_duration_ms("idle_timeout");
        if (it.has_value()) {
            idle_timeout_ms = *it;
        }

        // N2: snapshot git state around the tool invocation so the engine's
        // status writer surfaces what landed during the stage — most useful
        // for build/test nodes that don't commit but produce log files.
        GitStateSnapshot before = GitStateRecorder::capture(working_dir);

        std::map<std::string, std::string> env_overrides;
        std::string stdin_data;
        auto result = runner_->run(cmd, args, working_dir, timeout_ms,
                                   env_overrides, stdin_data, idle_timeout_ms);
        if (!result.ok()) {
            return Result<Outcome>::failure("process runner failed: " + result.error());
        }

        ProcessResult proc = result.value();

        Outcome outcome;
        if (proc.exit_code == 0 && !proc.timed_out) {
            outcome.status = StageStatus::SUCCESS;
        } else {
            outcome.status = StageStatus::FAILURE;
        }
        outcome.output = proc.stdout_output;
        outcome.context_updates["tool." + node.id + ".stdout"] = proc.stdout_output;
        outcome.context_updates["tool." + node.id + ".stderr"] = proc.stderr_output;
        outcome.context_updates["tool." + node.id + ".exit_code"] = std::to_string(proc.exit_code);
        if (proc.timed_out) {
            const char* kind = "wall_clock";
            if (proc.timeout_kind == TimeoutKind::Idle) kind = "idle";
            outcome.context_updates["tool." + node.id + ".timeout_kind"] = kind;
        }

        // N2: serialise git delta into context updates.
        GitStateSnapshot after = GitStateRecorder::capture(working_dir);
        if (before.valid && after.valid) {
            GitStateDelta delta = GitStateRecorder::diff(before, after, working_dir);
            outcome.context_updates["tool." + node.id + ".git_state"] =
                GitStateRecorder::to_json(delta).dump();
        }

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<ProcessRunner> runner_;
};

std::shared_ptr<Handler> make_tool_handler(std::shared_ptr<ProcessRunner> runner) {
    return std::make_shared<ToolHandler>(std::move(runner));
}

} // namespace needle
