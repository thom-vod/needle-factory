#include "needle/handlers/all_handlers.h"
#include "needle/handlers/handler_base.h"
#include "needle/backend/process_runner.h"
#include "needle/util/fs_helpers.h"

#include <memory>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "needle/platform/platform.h"

namespace needle {

namespace {

std::string detect_fetch_type(const std::string& url) {
    if (url.find(".git") != std::string::npos ||
        url.find("github.com") != std::string::npos ||
        url.find("gitlab.com") != std::string::npos) {
        return "git";
    }
    if (url.size() > 4 && url.substr(url.size() - 4) == ".pdf") {
        return "pdf";
    }
    return "web";
}

std::string read_file_contents(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool matches_pattern(const std::string& filename, const std::string& pattern) {
    // Simple glob: *.ext matching
    if (pattern.empty()) return true;
    if (pattern[0] == '*' && pattern.size() > 1) {
        std::string ext = pattern.substr(1);
        if (filename.size() >= ext.size() &&
            filename.substr(filename.size() - ext.size()) == ext) {
            return true;
        }
        return false;
    }
    return filename.find(pattern) != std::string::npos;
}

} // anonymous namespace

class DocFetchHandler : public HandlerBase {
public:
    explicit DocFetchHandler(std::shared_ptr<ProcessRunner> runner)
        : runner_(std::move(runner)) {}

    std::string type_name() const override { return "doc_fetch"; }

    Result<Outcome> do_execute(const Node& node, Context& /*ctx*/,
                               const ExecutionContext& exec_ctx) override {
        std::string url = node.attrs.get("url");
        if (url.empty() || url.find("$var.") != std::string::npos) {
            // No URL provided or unresolved variable — skip gracefully
            Outcome outcome;
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "doc_fetch skipped: no URL provided";
            outcome.context_updates["doc_fetch." + node.id + ".skipped"] = "true";
            outcome.context_updates["doc_fetch." + node.id + ".content"] = "";
            return Result<Outcome>::success(std::move(outcome));
        }

        std::string fetch_type = node.attrs.get("fetch_type");
        if (fetch_type.empty()) {
            fetch_type = detect_fetch_type(url);
        }

        std::string include_pattern = node.attrs.get("include_pattern");
        std::string max_size_str = node.attrs.get("max_size", "10485760"); // 10MB default
        std::string depth_str = node.attrs.get("depth", "1");

        // Ensure stage directory
        std::string stage_dir;
        if (!exec_ctx.logs_root.empty()) {
            stage_dir = exec_ctx.logs_root + "/stages/" + node.id;
            platform::mkdir_p(stage_dir);
        }

        int timeout_ms = 120000; // 2 minutes default
        Maybe<int> t = node.attrs.get_duration_ms("timeout");
        if (t.has_value()) {
            timeout_ms = *t;
        }

        Outcome outcome;

        if (fetch_type == "git") {
            outcome = fetch_git(node, url, stage_dir, depth_str, include_pattern, timeout_ms);
        } else if (fetch_type == "pdf") {
            outcome = fetch_pdf(node, url, stage_dir, timeout_ms);
        } else {
            outcome = fetch_web(node, url, stage_dir, timeout_ms);
        }

        return Result<Outcome>::success(std::move(outcome));
    }

private:
    Outcome fetch_git(const Node& node, const std::string& url,
                      const std::string& stage_dir, const std::string& depth,
                      const std::string& include_pattern, int timeout_ms) {
        std::string repo_dir = stage_dir.empty()
            ? platform::path_join(platform::temp_dir(), "needle_doc_fetch_" + node.id)
            : stage_dir + "/repo";

        std::vector<std::string> args = {
            "clone", "--depth", depth, url, repo_dir
        };

        auto result = runner_->run("git", args, ".", timeout_ms);

        Outcome outcome;
        if (!result.ok() || result.value().exit_code != 0) {
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "doc_fetch git clone failed for " + url + " (continuing with empty results)";
            outcome.context_updates["doc_fetch." + node.id + ".skipped"] = "true";
            outcome.context_updates["doc_fetch." + node.id + ".content"] = outcome.output;
            return outcome;
        }

        // List files and extract content
        std::vector<std::string> files;
        platform::list_files_recursive(repo_dir, "", files, 5);

        // Filter by include pattern (comma-separated)
        std::vector<std::string> patterns;
        if (!include_pattern.empty()) {
            std::istringstream iss(include_pattern);
            std::string pat;
            while (std::getline(iss, pat, ',')) {
                if (!pat.empty()) patterns.push_back(pat);
            }
        }

        std::ostringstream content;
        nlohmann::json manifest = nlohmann::json::array();
        size_t total_size = 0;
        const size_t max_content = 50 * 1024; // 50KB limit for context

        for (const auto& file : files) {
            // Filter
            if (!patterns.empty()) {
                bool match = false;
                for (const auto& p : patterns) {
                    if (matches_pattern(file, p)) { match = true; break; }
                }
                if (!match) continue;
            }

            std::string full_path = repo_dir + "/" + file;
            std::string text = read_file_contents(full_path);
            manifest.push_back({{"path", file}, {"size", text.size()}});

            if (total_size + text.size() <= max_content) {
                content << "### " << file << "\n```\n" << text << "\n```\n\n";
                total_size += text.size();
            }
        }

        // Write artifacts
        if (!stage_dir.empty()) {
            {
                std::ofstream out(stage_dir + "/content.md");
                if (out.is_open()) out << content.str();
            }
            {
                std::ofstream out(stage_dir + "/manifest.json");
                if (out.is_open()) out << manifest.dump(2);
            }
        }

        outcome.status = StageStatus::SUCCESS;
        outcome.output = content.str();
        outcome.context_updates["doc_fetch." + node.id + ".content"] = content.str();
        outcome.context_updates["doc_fetch." + node.id + ".files"] = manifest.dump();
        outcome.context_updates["doc_fetch." + node.id + ".url"] = url;

        return outcome;
    }

    Outcome fetch_web(const Node& node, const std::string& url,
                      const std::string& stage_dir, int timeout_ms) {
        std::vector<std::string> args = {"-sL", url};

        auto result = runner_->run("curl", args, ".", timeout_ms);

        Outcome outcome;
        if (!result.ok() || result.value().exit_code != 0) {
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "doc_fetch web fetch failed for " + url + " (continuing with empty results)";
            outcome.context_updates["doc_fetch." + node.id + ".skipped"] = "true";
            outcome.context_updates["doc_fetch." + node.id + ".content"] = outcome.output;
            return outcome;
        }

        std::string content = result.value().stdout_output;

        // Basic HTML to text: strip tags
        std::string text;
        bool in_tag = false;
        bool in_script = false;
        for (size_t i = 0; i < content.size(); ++i) {
            if (content[i] == '<') {
                // Check for script/style start
                std::string lower;
                for (size_t j = i; j < content.size() && j < i + 10; ++j) {
                    lower += static_cast<char>(std::tolower(content[j]));
                }
                if (lower.find("<script") == 0 || lower.find("<style") == 0) {
                    in_script = true;
                }
                if (lower.find("</script") == 0 || lower.find("</style") == 0) {
                    in_script = false;
                }
                in_tag = true;
            } else if (content[i] == '>') {
                in_tag = false;
            } else if (!in_tag && !in_script) {
                text += content[i];
            }
        }

        // Truncate to 50KB
        if (text.size() > 50 * 1024) {
            text = text.substr(0, 50 * 1024) + "\n\n[truncated]";
        }

        if (!stage_dir.empty()) {
            std::ofstream out(stage_dir + "/content.md");
            if (out.is_open()) out << text;
        }

        outcome.status = StageStatus::SUCCESS;
        outcome.output = text;
        outcome.context_updates["doc_fetch." + node.id + ".content"] = text;
        outcome.context_updates["doc_fetch." + node.id + ".url"] = url;

        return outcome;
    }

    Outcome fetch_pdf(const Node& node, const std::string& url,
                      const std::string& stage_dir, int timeout_ms) {
        Outcome outcome;

        // Download PDF
        std::string pdf_path = stage_dir.empty()
            ? platform::path_join(platform::temp_dir(), "needle_pdf_" + node.id + ".pdf")
            : stage_dir + "/doc.pdf";
        if (!stage_dir.empty()) platform::mkdir_p(stage_dir);

        std::vector<std::string> dl_args = {"-sL", "-o", pdf_path, url};
        auto dl_result = runner_->run("curl", dl_args, ".", timeout_ms);

        if (!dl_result.ok() || dl_result.value().exit_code != 0) {
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "doc_fetch PDF download failed for " + url + " (continuing with empty results)";
            outcome.context_updates["doc_fetch." + node.id + ".skipped"] = "true";
            outcome.context_updates["doc_fetch." + node.id + ".content"] = outcome.output;
            return outcome;
        }

        // Try pdftotext
        std::string text_path = stage_dir.empty()
            ? platform::path_join(platform::temp_dir(), "needle_pdf_" + node.id + ".txt")
            : stage_dir + "/content.md";
        std::vector<std::string> pdf_args = {pdf_path, text_path};
        auto pdf_result = runner_->run("pdftotext", pdf_args, ".", timeout_ms);

        if (!pdf_result.ok() || pdf_result.value().exit_code != 0) {
            outcome.status = StageStatus::SUCCESS;
            outcome.output = "doc_fetch: PDF downloaded but pdftotext not available (continuing without text extraction)";
            outcome.context_updates["doc_fetch." + node.id + ".skipped"] = "true";
            outcome.context_updates["doc_fetch." + node.id + ".content"] = outcome.output;
            return outcome;
        }

        std::string text = read_file_contents(text_path);
        if (text.size() > 50 * 1024) {
            text = text.substr(0, 50 * 1024) + "\n\n[truncated]";
        }

        outcome.status = StageStatus::SUCCESS;
        outcome.output = text;
        outcome.context_updates["doc_fetch." + node.id + ".content"] = text;
        outcome.context_updates["doc_fetch." + node.id + ".url"] = url;

        return outcome;
    }

    std::shared_ptr<ProcessRunner> runner_;
};

std::shared_ptr<Handler> make_doc_fetch_handler(std::shared_ptr<ProcessRunner> runner) {
    return std::make_shared<DocFetchHandler>(std::move(runner));
}

} // namespace needle
