#include "needle/engine/troubleshoot_agent.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "needle/platform/platform.h"
#include "needle/troubleshoot/allowed_tools.h"

namespace needle {

namespace {

std::string read_tail(const std::string& path, size_t bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) return "";
    std::streamoff start = size > static_cast<std::streamoff>(bytes)
        ? size - static_cast<std::streamoff>(bytes) : 0;
    in.seekg(start, std::ios::beg);
    std::string out(static_cast<size_t>(size - start), '\0');
    in.read(&out[0], static_cast<std::streamsize>(out.size()));
    return out;
}

void parse_final_result_event(const std::string& stdout_text, TroubleshootAgentResult& out) {
    std::istringstream in(stdout_text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            if (j.value("type", "") != "result") continue;
            if (j.contains("total_cost_usd") && j["total_cost_usd"].is_number()) {
                out.cost_usd = j["total_cost_usd"].get<double>();
            }
            if (j.contains("result") && j["result"].is_string()) {
                out.reasoning = j["result"].get<std::string>();
            }
            out.status = (j.value("subtype", "") == "success" && !j.value("is_error", false))
                ? TroubleshootSessionStatus::Resumed
                : TroubleshootSessionStatus::FailedAgent;
        } catch (const std::exception&) {
            continue;
        }
    }
}

} // namespace

TroubleshootAgentResult TroubleshootAgent::run(const std::string& node_id,
                                               const std::string& run_dir,
                                               const std::string& project_dir,
                                               const std::string& graph_path,
                                               const DiagnosisReport& report,
                                               Context& ctx,
                                               TroubleshootMode mode,
                                               std::shared_ptr<ProcessRunner> runner,
                                               int timeout_ms) {
    TroubleshootAgentResult out;
    if (!runner) runner = std::make_shared<NativeProcessRunner>();

    std::ostringstream prompt;
    prompt << "Troubleshoot failed stage '" << node_id << "'.\n";
    prompt << "Mode: " << to_string(mode) << ". Work within the allowed tools and leave a recovery report when useful.\n\n";
    prompt << "## Diagnosis\n" << Diagnose::render_markdown(report) << "\n\n";
    prompt << "## prompt.md tail\n" << read_tail(run_dir + "/stages/" + node_id + "/prompt.md", 4096) << "\n\n";
    prompt << "## response.md tail\n" << read_tail(run_dir + "/stages/" + node_id + "/response.md", 4096) << "\n\n";
    prompt << "## status.json\n" << read_tail(run_dir + "/stages/" + node_id + "/status.json", 4096) << "\n\n";
    prompt << "## run log tail\n" << read_tail(run_dir + "/run.log", 2048) << "\n\n";

    ctx.set("needle.project_dir", project_dir);
    if (!graph_path.empty()) ctx.set("needle.graph_path", graph_path);

    std::vector<std::string> args;
    if (mode == TroubleshootMode::Full) {
        args.push_back("--dangerously-skip-permissions");
    } else {
        args.push_back("--permission-mode");
        args.push_back("default");
        args.push_back("--allowed-tools");
        args.push_back(build_allowed_tools(
            mode, project_dir, graph_path, run_dir + "/troubleshoot/session-current"));
    }
    args.push_back("--model");
    args.push_back("claude-opus-4-7");
    args.push_back("--output-format");
    args.push_back("stream-json");
    args.push_back("--verbose");
    args.push_back("-p");
    args.push_back(prompt.str());

    auto r = runner->run("claude", args, project_dir.empty() ? "." : project_dir, timeout_ms);
    if (!r.ok()) {
        out.error = r.error();
        return out;
    }

    const ProcessResult& pr = r.value();
    out.exit_code = pr.exit_code;
    out.stdout_output = pr.stdout_output;
    out.stderr_output = pr.stderr_output;
    out.timed_out = pr.timed_out;
    if (pr.timed_out) {
        out.status = TroubleshootSessionStatus::FailedTimeout;
        out.error = "agent timed out";
        return out;
    }

    parse_final_result_event(pr.stdout_output, out);
    if (out.reasoning.empty()) out.reasoning = pr.stdout_output;
    out.ok = (pr.exit_code == 0);
    if (!out.ok) {
        out.status = TroubleshootSessionStatus::FailedAgent;
        out.error = pr.stderr_output.empty() ? "agent exited non-zero" : pr.stderr_output;
    }
    return out;
}

} // namespace needle
