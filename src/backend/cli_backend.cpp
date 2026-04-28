#include "needle/backend/cli_backend.h"
#include "needle/platform/portable_time.h"
#include "needle/util/context_expand.h"
#include "needle/util/fs_helpers.h"
#include "needle/util/logger.h"
#include "needle/util/uuid.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include "needle/platform/platform.h"

namespace needle {

namespace {

struct RateLimitInfo {
    bool detected = false;
    int wait_ms = 0;
    std::string retry_after;  // Human-readable time description
};

RateLimitInfo detect_rate_limit(const std::string& stdout_out, const std::string& stderr_out) {
    std::string combined = stdout_out + "\n" + stderr_out;

    // Case-insensitive search
    std::string lower = combined;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    RateLimitInfo info;

    bool has_pattern =
        lower.find("rate_limit") != std::string::npos ||
        lower.find("rate limit") != std::string::npos ||
        lower.find("usage limit") != std::string::npos ||
        lower.find("hit your usage") != std::string::npos ||
        lower.find("you've exceeded") != std::string::npos ||
        lower.find("too many requests") != std::string::npos ||
        lower.find("overloaded") != std::string::npos;

    if (!has_pattern) return info;
    info.detected = true;

    // Try to parse relative wait: "try again in X.Xs"
    auto pos = combined.find("try again in ");
    if (pos != std::string::npos) {
        try {
            double seconds = std::stod(combined.substr(pos + 13));
            info.wait_ms = static_cast<int>(seconds * 1000) + 500;
            if (info.wait_ms < 1000) info.wait_ms = 1000;
        } catch (...) {}
    }

    // Try to parse "try again after <timestring>"
    if (info.wait_ms == 0) {
        pos = combined.find("try again after ");
        if (pos != std::string::npos) {
            size_t start = pos + 16;
            size_t end = combined.find_first_of(".\n\r", start);
            if (end == std::string::npos) end = combined.size();
            std::string ts = combined.substr(start, end - start);
            while (!ts.empty() && std::isspace(static_cast<unsigned char>(ts.back())))
                ts.pop_back();
            if (!ts.empty()) info.retry_after = ts;
        }
    }

    // Try to parse "try again at <datetime>" (e.g. Codex: "Apr 15th, 2026 1:41 AM")
    if (info.wait_ms == 0) {
        pos = combined.find("try again at ");
        if (pos != std::string::npos) {
            size_t start = pos + 13;
            // Extract until period or newline
            size_t end = combined.find_first_of(".\n\r", start);
            if (end == std::string::npos) end = combined.size();
            std::string ts = combined.substr(start, end - start);
            while (!ts.empty() && std::isspace(static_cast<unsigned char>(ts.back())))
                ts.pop_back();
            if (!ts.empty()) {
                info.retry_after = ts;
                // Parse "Mon DDth, YYYY H:MM AM/PM" format
                // Remove ordinal suffixes (st, nd, rd, th) from day
                std::string clean = ts;
                for (const char* suf : {"th,", "st,", "nd,", "rd,"}) {
                    size_t p = clean.find(suf);
                    if (p != std::string::npos) {
                        clean.replace(p, 3, ",");
                    }
                }
                // Try parsing with strptime. Codex uses two forms:
                //   1. Full: "Apr 15, 2026 1:41 AM"
                //   2. Time-only: "6:26 PM" — interpret as the next
                //      occurrence of that wall-clock time.
#ifndef _WIN32
                std::time_t now = std::time(nullptr);
                std::time_t target = 0;

                struct tm tm_parsed;
                std::memset(&tm_parsed, 0, sizeof(tm_parsed));
                if (strptime(clean.c_str(), "%b %d, %Y %I:%M %p", &tm_parsed)) {
                    target = std::mktime(&tm_parsed);
                } else {
                    // Fallback: time-only "6:26 PM" or "06:26 PM".
                    struct tm tm_time;
                    std::memset(&tm_time, 0, sizeof(tm_time));
                    if (strptime(clean.c_str(), "%I:%M %p", &tm_time)) {
                        struct tm today;
                        localtime_r(&now, &today);
                        today.tm_hour = tm_time.tm_hour;
                        today.tm_min  = tm_time.tm_min;
                        today.tm_sec  = 0;
                        target = std::mktime(&today);
                        // If that moment has already passed today, the next
                        // occurrence is tomorrow at the same wall-clock time.
                        if (target > 0 && target <= now) {
                            target += 86400;
                        }
                    }
                }

                if (target > 0) {
                    double diff_s_d = std::difftime(target, now);
                    // Clamp to 24h to avoid int overflow on absurd dates
                    // (usage limits realistically reset within a day).
                    if (diff_s_d > 86400.0) diff_s_d = 86400.0;
                    int diff_s = static_cast<int>(diff_s_d);
                    if (diff_s > 0) {
                        info.wait_ms = diff_s * 1000 + 5000;  // +5s buffer
                    } else {
                        // Already past — try immediately
                        info.wait_ms = 1000;
                    }
                }
#endif
            }
        }
    }

    // If we parsed wait_ms but no retry_after string, compute one
    if (info.wait_ms > 0 && info.retry_after.empty()) {
        auto now = std::chrono::system_clock::now();
        auto resume = now + std::chrono::milliseconds(info.wait_ms);
        std::time_t resume_t = std::chrono::system_clock::to_time_t(resume);
        struct tm tm_buf;
        localtime_r(&resume_t, &tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
        info.retry_after = buf;
    }

    // Default wait for rate limits when no time was parseable
    if (info.wait_ms == 0) info.wait_ms = 60000;

    return info;
}

std::string replace_all(const std::string& str, const std::string& from, const std::string& to) {
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

} // close anonymous namespace so expand_context_refs has external linkage

// Runtime expansion of $context.key references against live context.
// Declared in needle/util/context_expand.h so other handlers can reuse it.
std::string expand_context_refs(const std::string& input, const Context& ctx) {
    std::string result;
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '$') {
            // Check for $context.key
            if (i + 9 < input.size() && input.substr(i + 1, 8) == "context.") {
                // Extract the key (dotted identifier)
                size_t start = i + 9;
                size_t end = start;
                while (end < input.size() &&
                       (std::isalnum(static_cast<unsigned char>(input[end])) ||
                        input[end] == '_' || input[end] == '.')) {
                    ++end;
                }
                std::string key = input.substr(start, end - start);
                if (!key.empty() && ctx.has(key)) {
                    result += ctx.get(key);
                    i = end;
                    continue;
                }
            }
            // Check for ${context.key}
            if (i + 1 < input.size() && input[i + 1] == '{') {
                size_t close = input.find('}', i + 2);
                if (close != std::string::npos) {
                    std::string var = input.substr(i + 2, close - i - 2);
                    if (var.size() > 8 && var.substr(0, 8) == "context.") {
                        std::string key = var.substr(8);
                        if (ctx.has(key)) {
                            result += ctx.get(key);
                            i = close + 1;
                            continue;
                        }
                    }
                }
            }
        }
        result += input[i];
        ++i;
    }
    return result;
}

namespace { // reopen anonymous namespace for remaining helpers

// Project-wide guardrails appended to every fidelity preamble. Sits right
// before the "---" separator the caller injects, so the model sees these
// directives immediately above the user prompt. Generalises a lesson from
// a real run that hung for 14+ minutes after the agent kicked off
// `find ~/.nuget/packages -name '*.dll' | xargs strings | grep`, burning
// through the stage timeout while the actual work was already done.
constexpr const char* kPreambleGuardrails =
    "## Guardrails\n\n"
    "Do not run brute-force scans over package caches or dependency "
    "directories — including but not limited to `~/.nuget/packages`, "
    "`node_modules`, `~/.cargo/registry`, `~/go/pkg/mod`, "
    "`~/.gradle/caches`, `~/Library/Caches/pip`, and language-specific "
    "site-packages. Commands like `find <cache> | xargs grep`, "
    "`find <cache> | xargs strings | grep`, or `grep -r` against the "
    "whole cache will hang the stage on any non-trivial project (these "
    "directories typically contain tens of thousands of files).\n\n"
    "If you need to verify a symbol or type exists in a dependency, run "
    "a TARGETED grep against a specific package subdirectory, not the "
    "whole cache. If a targeted grep against the obvious package finds "
    "nothing, assume the symbol does not exist and proceed — do not "
    "escalate to a full-cache scan.\n\n";

// Build a context preamble based on the resolved fidelity mode.
// The preamble provides upstream context to the LLM at varying levels of detail.
std::string build_fidelity_preamble(const std::string& mode, const Node& node,
                                     const Context& ctx) {
    std::string preamble;
    std::string goal = ctx.get("needle.goal");
    std::string node_label = node.label();

    // Resolve unknown modes to compact up front so the guardrails appendage
    // at the bottom of this function runs exactly once.
    std::string effective = mode;
    if (effective != "truncate" && effective != "compact" &&
        effective != "summary:high" && effective != "summary:medium" &&
        effective != "summary:low" && effective != "full") {
        effective = "compact";
    }

    if (effective == "truncate") {
        // Minimal: just goal and current node
        preamble = "## Context\n\nGoal: " + goal + "\nCurrent stage: " + node_label + "\n\n";
    }
    else if (effective == "compact") {
        // Structured summary: goal + completed stages + key context values
        preamble = "## Context\n\nGoal: " + goal + "\nCurrent stage: " + node_label + "\n\n";
        preamble += "### Completed stages\n";
        for (const auto& kv : ctx.all()) {
            if (kv.first.find(".output") != std::string::npos ||
                kv.first.find(".results") != std::string::npos) {
                std::string val = kv.second.substr(0, 200);
                if (kv.second.size() > 200) val += "...";
                preamble += "- **" + kv.first + "**: " + val + "\n";
            }
        }
        preamble += "\n";
    }
    else if (effective == "summary:high") {
        // Detailed: all non-internal context (~3000 tokens budget)
        preamble = "## Context\n\nGoal: " + goal + "\nCurrent stage: " + node_label + "\n\n";
        preamble += "### Prior stage outputs\n";
        for (const auto& kv : ctx.all()) {
            if (kv.first.find("internal.") == 0) continue;
            if (kv.first.find("needle.") == 0) continue;
            std::string val = kv.second.substr(0, 500);
            if (kv.second.size() > 500) val += "...";
            preamble += "- **" + kv.first + "**: " + val + "\n";
        }
        preamble += "\n";
    }
    else if (effective == "summary:medium") {
        // Moderate detail (~1500 tokens)
        preamble = "## Context\n\nGoal: " + goal + "\nCurrent stage: " + node_label + "\n\n";
        for (const auto& kv : ctx.all()) {
            if (kv.first.find("internal.") == 0) continue;
            if (kv.first.find("needle.") == 0) continue;
            if (kv.first.find("var.") == 0 || kv.first.find(".output") != std::string::npos) {
                std::string val = kv.second.substr(0, 300);
                if (kv.second.size() > 300) val += "...";
                preamble += "- **" + kv.first + "**: " + val + "\n";
            }
        }
        preamble += "\n";
    }
    else if (effective == "summary:low") {
        // Brief (~600 tokens)
        preamble = "## Context\n\nGoal: " + goal + "\nCurrent stage: " + node_label + "\n\n";
        preamble += "Prior stages completed. Key variables:\n";
        for (const auto& kv : ctx.all()) {
            if (kv.first.find("var.") == 0) {
                std::string val = kv.second.substr(0, 100);
                if (kv.second.size() > 100) val += "...";
                preamble += "- " + kv.first + ": " + val + "\n";
            }
        }
        preamble += "\n";
    }
    else if (effective == "full") {
        // Everything
        preamble = "## Full Context\n\nGoal: " + goal + "\nCurrent stage: " + node_label + "\n\n";
        for (const auto& kv : ctx.all()) {
            if (kv.first.find("needle.") == 0) continue;
            preamble += "- **" + kv.first + "**: " + kv.second + "\n";
        }
        preamble += "\n";
    }

    preamble += kPreambleGuardrails;
    return preamble;
}

} // anonymous namespace

// CLITemplate factory methods

CLITemplate CLITemplate::claude_default() {
    CLITemplate t;
    // No --output-format json: plain text streams incrementally so we capture
    // partial output on timeout. JSON mode buffers everything until completion.
    t.command_pattern = "claude -p --model {model} --session-id {session_id} --dangerously-skip-permissions --verbose";
    t.provider = "claude";
    t.defaults["model"] = "claude-opus-4-7";
    t.pipe_prompt_via_stdin = true;
    t.default_timeout_ms = 2700000;  // 45 minutes
    t.supports_resume = true;
    return t;
}

CLITemplate CLITemplate::codex_default() {
    CLITemplate t;
    t.command_pattern = "codex exec -m {model} -s danger-full-access";
    t.provider = "codex";
    t.defaults["model"] = "gpt-5.4";
    t.pipe_prompt_via_stdin = true;
    t.default_timeout_ms = 2700000;  // 45 minutes
    return t;
}

CLITemplate CLITemplate::gemini_default() {
    CLITemplate t;
    t.command_pattern = "gemini --yolo --model {model} --prompt .";
    t.provider = "gemini";
    t.defaults["model"] = "gemini-2.5-pro";
    t.pipe_prompt_via_stdin = true;
    t.default_timeout_ms = 2700000;  // 45 minutes
    return t;
}

// CLIBackend

CLIBackend::CLIBackend(CLITemplate tmpl, std::shared_ptr<ProcessRunner> runner)
    : default_template_(std::move(tmpl))
    , runner_(std::move(runner))
{
}

CLIBackend::CLIBackend(CLITemplate default_tmpl,
                       std::map<std::string, CLITemplate> templates,
                       std::shared_ptr<ProcessRunner> runner)
    : default_template_(std::move(default_tmpl))
    , templates_(std::move(templates))
    , runner_(std::move(runner))
{
}

std::string CLIBackend::name() const {
    return "cli:" + default_template_.provider;
}

const CLITemplate& CLIBackend::resolve_template(const Node& node) const {
    // Check node attribute "llm_provider" or "agent" to select a template
    std::string provider = node.attrs.get("llm_provider");
    if (provider.empty()) {
        provider = node.attrs.get("agent");
    }
    if (!provider.empty()) {
        auto it = templates_.find(provider);
        if (it != templates_.end()) {
            NEEDLE_LOG_DEBUG("cli", "node %s: resolved provider '%s' from node attr",
                             node.id.c_str(), provider.c_str());
            return it->second;
        }
        NEEDLE_LOG_DEBUG("cli", "node %s: provider '%s' not found in templates, using default",
                         node.id.c_str(), provider.c_str());
    } else {
        NEEDLE_LOG_DEBUG("cli", "node %s: no llm_provider attr, using default '%s'",
                         node.id.c_str(), default_template_.provider.c_str());
    }
    return default_template_;
}

std::string CLIBackend::build_command(const CLITemplate& tmpl, const Node& node,
                                       const Context& ctx, const std::string& stage_dir)
{
    (void)ctx;
    std::string pattern = tmpl.command_pattern;

    // Get model: node attr > template default
    std::string model = node.attrs.get("llm_model");
    if (model.empty()) {
        auto it = tmpl.defaults.find("model");
        if (it != tmpl.defaults.end()) {
            model = it->second;
        }
    }

    std::string prompt_file = stage_dir + "/prompt.md";
    std::string working_dir = node.attrs.get("working_dir", ".");

    pattern = replace_all(pattern, "{model}", model);
    // {session_id} is replaced by the caller (execute method)
    // Leave it as-is here — it will be substituted in build_args
    pattern = replace_all(pattern, "{prompt_file}", prompt_file);
    pattern = replace_all(pattern, "{working_dir}", working_dir);
    pattern = replace_all(pattern, "{provider}", tmpl.provider);

    return pattern;
}

std::vector<std::string> CLIBackend::build_args(const CLITemplate& tmpl, const Node& node,
                                                  const Context& ctx, const std::string& stage_dir)
{
    std::string cmd = build_command(tmpl, node, ctx, stage_dir);
    // Split by spaces (simple tokenization)
    std::vector<std::string> tokens;
    std::istringstream iss(cmd);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

Result<Outcome> CLIBackend::execute(const Node& node, Context& ctx,
                                     const std::string& stage_dir)
{
    // Create stage directory and archive any existing results from prior runs
    platform::mkdir_p(stage_dir);
    archive_stage_files(stage_dir);

    // Write prompt to stage_dir/prompt.md
    std::string prompt = node.prompt();
    if (prompt.empty()) {
        prompt = node.label();
    }

    // Runtime expansion of $context.* references against live context
    prompt = expand_context_refs(prompt, ctx);

    // Resolve and inject fidelity preamble before the prompt
    {
        std::string fidelity_mode = ctx.get("needle.fidelity_mode");
        if (fidelity_mode.empty()) {
            fidelity_mode = "compact";
        }
        std::string preamble = build_fidelity_preamble(fidelity_mode, node, ctx);
        if (!preamble.empty()) {
            prompt = preamble + "---\n\n" + prompt;
        }
    }

    // Append human gate feedback if present in context
    std::string feedback = ctx.get("human.gate.feedback");
    if (!feedback.empty()) {
        prompt += "\n\n---\nFeedback from reviewer: " + feedback;
    }

    // Add workflow instructions for codergen nodes
    {
        // General instruction: commit after completing work
        if (node.attrs.get("no_commit") != "true") {
            prompt += "\n\n---\n"
                "## Workflow Instructions\n\n"
                "After completing your implementation:\n"
                "1. Run the project's build command to verify compilation\n"
                "2. Run the project's test suite to verify no regressions\n"
                "3. Commit your changes with a clear, descriptive commit message summarizing what was done\n"
                "4. Do NOT modify or write to the .needle/ directory — it is managed by the pipeline engine\n";
        }

        // For complex tasks, add skill instructions
        bool use_skills = (node.attrs.get("use_skills") == "true");
        if (!use_skills) {
            Maybe<int> t = node.attrs.get_duration_ms("timeout");
            NEEDLE_LOG_DEBUG("cli", "node %s: use_skills check: attr=%s, timeout has_value=%s, val=%d",
                             node.id.c_str(), node.attrs.get("use_skills").c_str(),
                             t.has_value() ? "true" : "false", t.has_value() ? *t : -1);
            if (t.has_value() && *t > 1800000) {  // explicit timeout > 30 min
                use_skills = true;
            }
        }
        if (use_skills) {
            prompt += "\n"
                "## Available Skills\n\n"
                "You have access to sprint planning and execution skills for complex, multi-step tasks:\n\n"
                "- **/sprint-plan** — Use this for tasks that require significant planning before implementation. "
                "It will help you break the work into phases, identify risks, and produce a structured plan.\n"
                "- **/sprint-execute** — Use this to execute a plan phase-by-phase with build/test validation after each phase.\n"
                "- **/review-code** — Use this for comprehensive multi-perspective code review.\n\n"
                "For this task, consider whether the scope is large enough to benefit from structured planning "
                "before diving into implementation. If the task involves multiple files, complex architecture decisions, "
                "or significant refactoring, use /sprint-plan first to create a plan, then /sprint-execute to implement it.\n\n"
                "IMPORTANT: You are running autonomously in a pipeline — there is no human to interact with.\n"
                "Do NOT ask for confirmation or wait for user input at any point.\n"
                "If you use /sprint-plan, immediately follow it with /sprint-execute to implement the plan.\n"
                "Make all decisions yourself. If something is ambiguous, choose the most reasonable option and proceed.\n\n"
                "When using sprint skills:\n"
                "- Commit at the end of each sprint phase with a message describing the phase\n"
                "- Commit at the end of the sprint with a summary message\n"
                "- Each commit should leave the build passing and tests green\n";
        }
    }
    {
        std::string prompt_path = stage_dir + "/prompt.md";
        std::ofstream out(prompt_path);
        if (!out.is_open()) {
            return Result<Outcome>::failure("failed to write prompt file: " + prompt_path);
        }
        out << prompt;
    }

    // Resolve which CLI template to use for this node
    const CLITemplate& tmpl = resolve_template(node);

    // Build command and args
    std::vector<std::string> tokens = build_args(tmpl, node, ctx, stage_dir);
    if (tokens.empty()) {
        return Result<Outcome>::failure("empty command");
    }

    std::string command = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());

    // Generate a fresh session ID for this execution (unique per attempt)
    std::string session_id = random_uuid();
    for (auto& a : args) {
        if (a == "{session_id}") a = session_id;
    }

    // Write session_id to stage dir BEFORE launching (for needle attach)
    if (tmpl.supports_resume && !stage_dir.empty()) {
        std::ofstream sid_out(stage_dir + "/session_id");
        if (sid_out.is_open()) sid_out << session_id;
    }

    // Get timeout: node attr > template default (45 min)
    int timeout_ms = tmpl.default_timeout_ms;
    Maybe<int> t = node.attrs.get_duration_ms("timeout");
    if (t.has_value()) {
        timeout_ms = *t;
    }
    NEEDLE_LOG_INFO("cli", "node %s: timeout=%dms (%dm), command=%s, provider=%s",
                    node.id.c_str(), timeout_ms, timeout_ms / 60000,
                    command.c_str(), tmpl.provider.c_str());

    // Pipe prompt via stdin if configured, otherwise prompt_file is in the command args
    std::string stdin_data;
    if (tmpl.pipe_prompt_via_stdin) {
        stdin_data = prompt;
    }

    // Note: session_id files in stage_dir are preserved for manual debugging
    // via "needle attach <node_id>", but automatic --resume is disabled.
    // Reasons: the old session may have wrong model, wrong instructions, or
    // be stuck waiting for input. A fresh start with the current prompt and
    // configuration is more reliable for pipeline retries.

    // Run process — project_dir from context overrides default "."
    std::string working_dir = node.attrs.get("working_dir");
    if (working_dir.empty()) {
        working_dir = ctx.get("needle.project_dir");
    }
    if (working_dir.empty()) {
        working_dir = ".";
    }
    std::map<std::string, std::string> env_overrides;

    if (wrapper_) {
        WrappedCommand wrapped = wrapper_(command, args, node, ctx);
        command = std::move(wrapped.command);
        args = std::move(wrapped.args);
        for (auto& kv : wrapped.env_overrides) {
            env_overrides[kv.first] = std::move(kv.second);
        }
    }

    auto result = runner_->run(command, args, working_dir, timeout_ms, env_overrides, stdin_data);
    if (!result.ok()) {
        return Result<Outcome>::failure("process runner failed: " + result.error());
    }

    ProcessResult proc = result.value();

    // Write response to stage_dir/response.md
    {
        std::string resp_path = stage_dir + "/response.md";
        std::ofstream out(resp_path);
        if (out.is_open()) {
            out << proc.stdout_output;
        }
    }

    // Build outcome
    Outcome outcome;

    // Always write stderr to debug.log (useful on timeout/failure)
    if (!proc.stderr_output.empty() && !stage_dir.empty()) {
        std::string debug_path = stage_dir + "/debug.log";
        std::ofstream dbg(debug_path);
        if (dbg.is_open()) {
            dbg << proc.stderr_output;
        }
    }

    NEEDLE_LOG_INFO("cli", "node %s: process finished: timed_out=%s exit_code=%d stdout=%zu stderr=%zu",
                    node.id.c_str(), proc.timed_out ? "true" : "false",
                    proc.exit_code, proc.stdout_output.size(), proc.stderr_output.size());

    if (proc.timed_out) {
        outcome.status = StageStatus::FAILURE;
        outcome.output = "proc timed out after " + std::to_string(timeout_ms / 1000) + "s"
            + (proc.stdout_output.empty() ? "" : " (partial output in response.md)");
    } else if (proc.exit_code == 0) {
        outcome.status = StageStatus::SUCCESS;
        std::string output = proc.stdout_output;
        // Try to extract result text from JSON output (if agent uses --output-format json)
        try {
            auto parsed = nlohmann::json::parse(output);
            if (parsed.count("result") && parsed["result"].is_string()) {
                output = parsed["result"].get<std::string>();
            }
        } catch (...) {
            // Not JSON — use raw text output (this is the normal case now)
        }
        outcome.output = output;
    } else {
        // Non-zero exit — check for rate/usage limit before treating as failure
        RateLimitInfo rl = detect_rate_limit(proc.stdout_output, proc.stderr_output);
        if (rl.detected) {
            outcome.status = StageStatus::RETRY;
            outcome.retry_after_ms = rl.wait_ms;
            std::string msg = "Usage limit reached";
            if (!rl.retry_after.empty()) {
                msg += ", please try again after " + rl.retry_after;
            }
            outcome.output = msg;
            NEEDLE_LOG_INFO("cli", "node %s: %s (retry_after_ms=%d)",
                            node.id.c_str(), msg.c_str(), rl.wait_ms);
        } else {
            outcome.status = StageStatus::FAILURE;
            outcome.output = proc.stderr_output.empty() ? proc.stdout_output : proc.stderr_output;
        }
    }

    // Record session_id in context for needle attach
    if (tmpl.supports_resume) {
        outcome.context_updates["cli.session_id." + node.id] = session_id;
    }

    // Check for richer outcome from status.json
    std::string status_path = stage_dir + "/status.json";
    std::ifstream status_in(status_path);
    if (status_in.is_open()) {
        try {
            nlohmann::json j;
            status_in >> j;
            if (j.count("status")) {
                outcome.status = stage_status_from_string(j["status"].get<std::string>());
            }
            if (j.count("preferred_label")) {
                outcome.preferred_label = j["preferred_label"].get<std::string>();
            }
            if (j.count("context_updates") && j["context_updates"].is_object()) {
                for (auto it = j["context_updates"].begin();
                     it != j["context_updates"].end(); ++it) {
                    outcome.context_updates[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (...) {
            // Ignore parse errors in status.json
        }
    }

    return Result<Outcome>::success(std::move(outcome));
}

} // namespace needle
