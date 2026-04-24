#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/backend/process_runner.h"
#include "needle/util/fs_helpers.h"
#include "needle/config/needle_config.h"

#include <memory>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "needle/platform/platform.h"
#include <nlohmann/json.hpp>

namespace needle {

namespace {

std::string get_env(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : "";
}

} // anonymous namespace

class WebSearchHandler : public HandlerBase {
public:
    explicit WebSearchHandler(std::shared_ptr<ProcessRunner> runner)
        : runner_(std::move(runner)) {}

    std::string type_name() const override { return "web_search"; }

    Result<Outcome> do_execute(const Node& node, Context& /*ctx*/,
                               const ExecutionContext& exec_ctx) override {
        // Get search query from node attributes
        std::string query = node.attrs.get("query");
        if (query.empty()) {
            query = node.prompt();
        }
        if (query.empty()) {
            query = node.label();
        }
        if (query.empty()) {
            return Result<Outcome>::failure("web_search node missing 'query' attribute: " + node.id);
        }

        // Get provider (only tavily supported this sprint)
        std::string provider = node.attrs.get("provider", "tavily");

        // Get max results
        std::string max_results_str = node.attrs.get("max_results", "5");
        int max_results = std::atoi(max_results_str.c_str());
        if (max_results <= 0) max_results = 5;

        // Check for API key — env var takes precedence, then config file
        std::string api_key;
        if (provider == "tavily") {
            api_key = get_env("TAVILY_API_KEY");
            if (api_key.empty()) {
                api_key = NeedleConfig::global().resolve_api_key("tavily");
            }
        }

        Outcome outcome;

        if (api_key.empty()) {
            // Graceful degradation — no API key
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "web_search skipped: " + provider + " API key not configured (set TAVILY_API_KEY)";
            outcome.context_updates["web_search." + node.id + ".skipped"] = "true";
            outcome.context_updates["web_search." + node.id + ".results"] = outcome.output;

            // Write to stage dir if available
            if (!exec_ctx.logs_root.empty()) {
                std::string stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
                platform::mkdir_p(stage_dir);
                std::ofstream out(stage_dir + "/results.md");
                if (out.is_open()) out << outcome.output;
            }

            return Result<Outcome>::success(std::move(outcome));
        }

        // Build Tavily API request
        nlohmann::json request_body;
        request_body["api_key"] = api_key;
        request_body["query"] = query;
        request_body["max_results"] = max_results;
        request_body["include_answer"] = true;

        std::string json_body = request_body.dump();

        // Build curl command
        std::vector<std::string> args = {
            "-s", "-X", "POST",
            "https://api.tavily.com/search",
            "-H", "Content-Type: application/json",
            "-d", json_body
        };

        int timeout_ms = 30000; // 30s default for search
        Maybe<int> t = node.attrs.get_duration_ms("timeout");
        if (t.has_value()) {
            timeout_ms = *t;
        }

        auto result = runner_->run("curl", args, ".", timeout_ms);
        if (!result.ok()) {
            // Graceful degradation — curl failed
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "web_search failed: " + result.error() + " (continuing with empty results)";
            outcome.context_updates["web_search." + node.id + ".skipped"] = "true";
            outcome.context_updates["web_search." + node.id + ".results"] = outcome.output;
            return Result<Outcome>::success(std::move(outcome));
        }

        ProcessResult proc = result.value();

        if (proc.exit_code != 0 || proc.timed_out) {
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "web_search failed: curl exit " + std::to_string(proc.exit_code) +
                             (proc.timed_out ? " (timed out)" : "") + " (continuing with empty results)";
            outcome.context_updates["web_search." + node.id + ".skipped"] = "true";
            outcome.context_updates["web_search." + node.id + ".results"] = outcome.output;
            return Result<Outcome>::success(std::move(outcome));
        }

        // Parse Tavily response
        std::string markdown_results;
        nlohmann::json response_json;
        int result_count = 0;

        try {
            response_json = nlohmann::json::parse(proc.stdout_output);

            // Extract answer if present
            if (response_json.count("answer") && response_json["answer"].is_string()) {
                markdown_results += "## Summary\n\n" + response_json["answer"].get<std::string>() + "\n\n";
            }

            // Extract results
            if (response_json.count("results") && response_json["results"].is_array()) {
                markdown_results += "## Search Results\n\n";
                for (const auto& r : response_json["results"]) {
                    std::string title = r.value("title", "");
                    std::string url = r.value("url", "");
                    std::string content = r.value("content", "");

                    markdown_results += "### " + title + "\n";
                    markdown_results += "**URL:** " + url + "\n\n";
                    markdown_results += content + "\n\n---\n\n";
                    ++result_count;
                }
            }
        } catch (const std::exception& e) {
            // JSON parse error — return raw output
            markdown_results = "## Raw Response (parse error: " + std::string(e.what()) + ")\n\n" +
                              proc.stdout_output;
        }

        // Write to stage directory
        if (!exec_ctx.logs_root.empty()) {
            std::string stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
            platform::mkdir_p(stage_dir);

            {
                std::ofstream out(stage_dir + "/results.md");
                if (out.is_open()) out << markdown_results;
            }
            {
                std::ofstream out(stage_dir + "/results.json");
                if (out.is_open()) out << proc.stdout_output;
            }
        }

        outcome.status = StageStatus::SUCCESS;
        outcome.output = markdown_results;
        outcome.context_updates["web_search." + node.id + ".results"] = markdown_results;
        outcome.context_updates["web_search." + node.id + ".count"] = std::to_string(result_count);
        outcome.context_updates["web_search." + node.id + ".query"] = query;

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    std::shared_ptr<ProcessRunner> runner_;
};

std::shared_ptr<Handler> make_web_search_handler(std::shared_ptr<ProcessRunner> runner) {
    return std::make_shared<WebSearchHandler>(std::move(runner));
}

} // namespace needle
