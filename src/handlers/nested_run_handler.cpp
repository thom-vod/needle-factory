#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/backend/process_runner.h"
#include "needle/util/fs_helpers.h"
#include "needle/util/utf8.h"

#include <memory>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <chrono>
#include <random>
#include "needle/platform/platform.h"

namespace needle {

namespace {

std::string generate_run_id() {
    static std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> dist(0, 15);
    const char* hex = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 8; ++i) {
        id += hex[dist(rng)];
    }
    return id;
}

void copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (in.is_open() && out.is_open()) {
        out << in.rdbuf();
    }
}

} // anonymous namespace

class NestedRunHandler : public HandlerBase {
public:
    explicit NestedRunHandler(std::shared_ptr<ProcessRunner> runner)
        : runner_(std::move(runner)) {}

    std::string type_name() const override { return "nested_run"; }

    Result<Outcome> do_execute(const Node& node, Context& ctx,
                               const ExecutionContext& exec_ctx) override {
        // Get DOT file path
        std::string dot_file = node.attrs.get("dot_file");
        if (dot_file.empty()) {
            // Try context key (e.g., from a prior codergen node that generated a DOT file)
            dot_file = ctx.get("nested_run." + node.id + ".dot_file");
        }
        if (dot_file.empty()) {
            return Result<Outcome>::failure("nested_run node missing 'dot_file' attribute: " + node.id);
        }

        // Resolve relative paths against project directory
        std::string project_dir = exec_ctx.project_dir.empty() ? "." : exec_ctx.project_dir;
        if (!platform::is_absolute_path(dot_file) && !project_dir.empty()) {
            dot_file = project_dir + "/" + dot_file;
        }

        if (!platform::file_exists(dot_file)) {
            return Result<Outcome>::failure("nested_run dot_file not found: " + dot_file);
        }

        // Check recursion depth
        std::string depth_str = ctx.get("needle.depth");
        int current_depth = depth_str.empty() ? 0 : std::atoi(depth_str.c_str());

        std::string max_depth_str = node.attrs.get("max_depth", "3");
        int max_depth = std::atoi(max_depth_str.c_str());

        if (current_depth >= max_depth) {
            Outcome outcome;
            outcome.status = StageStatus::FAILURE;
            outcome.output = "nested_run recursion depth limit reached (" +
                             std::to_string(current_depth) + "/" + std::to_string(max_depth) + ")";
            return Result<Outcome>::success(std::move(outcome));
        }

        // Create isolated subdirectory under the project directory
        std::string run_id = generate_run_id();
        std::string run_rel = ".needle/runs/" + run_id;
        std::string run_dir = project_dir + "/" + run_rel;
        platform::mkdir_p(run_dir);

        // Copy DOT file to run directory
        std::string run_dot = run_dir + "/pipeline.dot";
        copy_file(dot_file, run_dot);

        // Copy stylesheet if specified
        std::string stylesheet = node.attrs.get("stylesheet");
        if (!stylesheet.empty() && !platform::is_absolute_path(stylesheet) && !project_dir.empty()) {
            stylesheet = project_dir + "/" + stylesheet;
        }
        std::string run_stylesheet;
        if (!stylesheet.empty() && platform::file_exists(stylesheet)) {
            run_stylesheet = run_dir + "/pipeline.nss";
            copy_file(stylesheet, run_stylesheet);
        }

        // Build needle command — use paths relative to project_dir (the working directory)
        std::vector<std::string> args;
        args.push_back("run");
        args.push_back(run_rel + "/pipeline.dot");
        args.push_back("--logs-dir");
        args.push_back(run_rel + "/.needle");
        args.push_back("--var");
        args.push_back("needle.depth=" + std::to_string(current_depth + 1));

        if (!run_stylesheet.empty()) {
            args.push_back("--stylesheet");
            args.push_back(run_rel + "/pipeline.nss");
        }

        // Get timeout
        int timeout_ms = 1800000; // 30 minutes default
        Maybe<int> t = node.attrs.get_duration_ms("timeout");
        if (t.has_value()) {
            timeout_ms = *t;
        }

        // Find needle binary — use the same binary that's running
        std::string needle_bin = "needle";

        std::string working_dir = exec_ctx.project_dir.empty() ? "." : exec_ctx.project_dir;
        auto result = runner_->run(needle_bin, args, working_dir, timeout_ms);

        Outcome outcome;
        if (!result.ok()) {
            outcome.status = StageStatus::FAILURE;
            outcome.output = "nested_run process failed: " + result.error();
            outcome.context_updates["nested_run." + node.id + ".success"] = "false";
            return Result<Outcome>::success(std::move(outcome));
        }

        ProcessResult proc = result.value();

        // Truncate output to 10KB
        std::string output = proc.stdout_output;
        if (output.size() > 10240) {
            output = utf8::truncate_back(output, 10240);
            output = "[truncated]\n" + output;
        }

        if (proc.exit_code == 0 && !proc.timed_out) {
            outcome.status = StageStatus::SUCCESS;
            outcome.output = output;
            outcome.context_updates["nested_run." + node.id + ".success"] = "true";
        } else {
            outcome.status = StageStatus::FAILURE;
            outcome.output = "nested_run failed: exit " + std::to_string(proc.exit_code) +
                             (proc.timed_out ? " (timed out)" : "") + "\n" + output;
            outcome.context_updates["nested_run." + node.id + ".success"] = "false";
        }

        outcome.context_updates["nested_run." + node.id + ".exit_code"] = std::to_string(proc.exit_code);
        outcome.context_updates["nested_run." + node.id + ".output"] = output;
        outcome.context_updates["nested_run." + node.id + ".run_dir"] = run_rel;

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<ProcessRunner> runner_;
};

std::shared_ptr<Handler> make_nested_run_handler(std::shared_ptr<ProcessRunner> runner) {
    return std::make_shared<NestedRunHandler>(std::move(runner));
}

} // namespace needle
