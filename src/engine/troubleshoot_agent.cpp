#include "needle/engine/troubleshoot_agent.h"

#include <fstream>
#include <sstream>

#include "needle/platform/platform.h"

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

std::string extract_last_stage_cmd(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::string last;
    while (std::getline(in, line)) {
        std::size_t p = line.find("needle stage ");
        if (p != std::string::npos) {
            last = line.substr(p);
        }
    }
    return last;
}

bool command_targets_node(const std::string& cmd, const std::string& node_id) {
    std::istringstream iss(cmd);
    std::vector<std::string> parts;
    std::string part;
    while (iss >> part) parts.push_back(part);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == node_id) return true;
        if (parts[i] == "--to" && i + 1 < parts.size() && parts[i + 1] == node_id) return true;
    }
    return false;
}

} // namespace

TroubleshootAgentResult TroubleshootAgent::run(const std::string& node_id,
                                               const std::string& run_dir,
                                               const std::string& project_dir,
                                               const DiagnosisReport& report,
                                               CLIBackend& backend,
                                               Context& ctx,
                                               int timeout_ms) {
    TroubleshootAgentResult out;

    Node n;
    n.id = "troubleshoot_" + node_id;
    n.type = NodeType::CODERGEN;
    n.attrs.set("class", "troubleshoot");
    n.attrs.set("agent", ctx.get("needle.troubleshoot_agent"));
    if (n.attrs.get("agent").empty()) {
        n.attrs.set("agent", "claude");
    }
    std::string model = ctx.get("needle.troubleshoot_model");
    if (!model.empty()) n.attrs.set("llm_model", model);
    n.attrs.set("allowed_tools", "Read,Bash,Glob,Grep");
    n.attrs.set("timeout", std::to_string(timeout_ms) + "ms");

    std::ostringstream prompt;
    prompt << "Troubleshoot failed stage '" << node_id << "'.\n";
    prompt << "Return by executing exactly one command: needle stage mark|advance|retry for this node.\n\n";
    prompt << "## Diagnosis\n" << Diagnose::render_markdown(report) << "\n\n";
    prompt << "## prompt.md tail\n" << read_tail(run_dir + "/stages/" + node_id + "/prompt.md", 4096) << "\n\n";
    prompt << "## response.md tail\n" << read_tail(run_dir + "/stages/" + node_id + "/response.md", 4096) << "\n\n";
    prompt << "## status.json\n" << read_tail(run_dir + "/stages/" + node_id + "/status.json", 4096) << "\n\n";
    prompt << "## run log tail\n" << read_tail(run_dir + "/run.log", 2048) << "\n\n";
    n.attrs.set("prompt", prompt.str());

    ctx.set("needle.project_dir", project_dir);
    auto r = backend.execute(n, ctx, run_dir + "/stages/" + n.id);
    if (!r.ok()) {
        out.error = r.error();
        return out;
    }

    const Outcome& outcome = r.value();
    if (outcome.status == StageStatus::FAILURE && outcome.output.find("timed out") != std::string::npos) {
        out.timed_out = true;
        out.error = "agent timed out";
        return out;
    }

    out.reasoning = outcome.output;
    out.command = extract_last_stage_cmd(outcome.output);
    if (out.command.empty()) {
        out.error = "agent did not emit a stage command";
        return out;
    }

    if (out.command.find("needle stage mark ") != 0 &&
        out.command.find("needle stage advance ") != 0 &&
        out.command.find("needle stage retry ") != 0) {
        out.error = "invalid terminal command";
        return out;
    }

    if (!command_targets_node(out.command, node_id)) {
        out.error = "terminal command targeted a different node";
        return out;
    }

    out.ok = true;
    return out;
}

} // namespace needle
