#ifdef NEEDLE_ENABLE_SERVER

#include "needle/server/dot_generator.h"
#include "needle/config/needle_config.h"
#include "needle/rules/dot_authoring_rules.h"
#include <nlohmann/json.hpp>
#include <cstdlib>

#ifdef NEEDLE_HAS_CURL
#include <curl/curl.h>
#endif

namespace needle {

namespace {

#ifdef NEEDLE_HAS_CURL
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    std::string* buf = static_cast<std::string*>(ud);
    size_t total = size * nmemb;
    buf->append(ptr, total);
    return total;
}
#endif

struct ProviderInfo {
    const char* base_url;
    const char* api_key_env;
    const char* default_model;
};

ProviderInfo get_provider(const std::string& name) {
    if (name == "openai") {
        return {"https://api.openai.com", "OPENAI_API_KEY", "gpt-4o"};
    } else if (name == "google") {
        return {"https://generativelanguage.googleapis.com", "GEMINI_API_KEY", "gemini-2.5-flash"};
    }
    // Default: anthropic
    return {"https://api.anthropic.com", "ANTHROPIC_API_KEY", "claude-sonnet-4-20250514"};
}

} // anonymous namespace

std::string DotGenerator::system_prompt() {
    // Read configured agent/model defaults for the model_stylesheet example
    auto& cfg = NeedleConfig::global();
    std::string coding_agent  = cfg.get_string("defaults.coding_agent",  "", "codex");
    std::string coding_model  = cfg.get_string("defaults.coding_model",  "", "gpt-5.4");
    std::string planning_agent = cfg.get_string("defaults.planning_agent", "", "claude");
    std::string planning_model = cfg.get_string("defaults.planning_model", "", "claude-opus-4-7");
    std::string critique_agent = cfg.get_string("defaults.critique_agent", "", "gemini");
    std::string critique_model = cfg.get_string("defaults.critique_model", "", "gemini-2.5-pro");

    return std::string(
        "You are a pipeline architect for needle. Generate valid full DOT graphs.\n\n"
        "Follow these canonical rules exactly:\n\n")
        + rules::kDotAuthoringRules +
        std::string("\n\n## Response format\n"
        "Return full DOT in a ```dot code block and include brief rationale before it.\n\n"
        "## Example\n"
        "```dot\n"
        "digraph pipeline {\n"
        "    graph [\n"
        "        goal=\"Build and test the project\",\n"
        "        label=\"Build Pipeline\",\n"
        "        default_max_retries=3,\n"
        "        model_stylesheet=\"\n"
        "            * { agent = \\\"" + coding_agent + "\\\"; llm_model = \\\"" + coding_model + "\\\" }\n"
        "            .planning { agent = \\\"" + planning_agent + "\\\"; llm_model = \\\"" + planning_model + "\\\"; reasoning_effort = \\\"high\\\" }\n"
        "            .coding { agent = \\\"" + coding_agent + "\\\"; llm_model = \\\"" + coding_model + "\\\" }\n"
        "            .critique { agent = \\\"" + critique_agent + "\\\"; llm_model = \\\"" + critique_model + "\\\" }\n"
        "        \"\n"
        "    ]\n"
        "\n"
        "    start [shape=Mdiamond, label=\"Start\"]\n"
        "    exit  [shape=Msquare, label=\"Done\"]\n"
        "\n"
        "    orient [class=\"planning\", label=\"Orient\", fidelity=\"truncate\",\n"
        "        prompt=\"Check for prior run artifacts ({{logs_dir}}/orient/*-*.md). "
            "Assess the workspace.\"]\n"
        "    plan [class=\"planning\", label=\"Plan\", fidelity=\"summary:high\",\n"
        "        prompt=\"Check for prior run artifacts ({{logs_dir}}/plan/*-*.md). "
            "Produce a detailed implementation plan.\"]\n"
        "    build [label=\"Build\", timeout=\"45m\", fidelity=\"full\",\n"
        "        prompt=\"Check for prior run artifacts ({{logs_dir}}/build/*-*.md). "
            "Implement per the plan. Commit after each logical unit.\"]\n"
        "    validate [label=\"Validate\", handler=\"tool\", goal_gate=true,\n"
        "        retry_target=\"fix\", fallback_retry_target=\"plan\",\n"
        "        command=\"make test 2>&1; echo EXIT_CODE=$?\"]\n"
        "    fix [label=\"Fix\", fidelity=\"summary:high\",\n"
        "        prompt=\"Check for prior run artifacts ({{logs_dir}}/fix/*-*.md). "
            "Fix all test failures. Commit.\"]\n"
        "    critique [class=\"critique\", label=\"Critique\", fidelity=\"summary:high\",\n"
        "        prompt=\"Check for prior run artifacts ({{logs_dir}}/critique/*-*.md). "
            "Review for security, edge cases, coverage gaps.\"]\n"
        "    write_readme [label=\"Write README\", fidelity=\"compact\",\n"
        "        prompt=\"Write README.md with description, setup, usage, architecture.\"]\n"
        "    review [shape=hexagon, label=\"Review\"]\n"
        "    apply_feedback [label=\"Apply Feedback\", fidelity=\"summary:high\",\n"
        "        prompt=\"Apply reviewer changes: $context.human.gate.feedback. Commit.\"]\n"
        "\n"
        "    start -> orient -> plan -> build -> validate\n"
        "    validate -> critique [label=\"pass\", condition=\"outcome=success\"]\n"
        "    validate -> fix [label=\"fail\", condition=\"outcome!=success\"]\n"
        "    fix -> validate\n"
        "    critique -> write_readme -> review\n"
        "    review -> exit [label=\"[A] Approve\"]\n"
        "    review -> apply_feedback [label=\"[C] Request changes\"]\n"
        "    apply_feedback -> validate\n"
        "}\n"
        "```\n");
}

std::string DotGenerator::build_anthropic_body(const std::string& sys_prompt,
                                                const std::vector<ChatMessage>& messages,
                                                const std::string& model) const {
    nlohmann::json body;
    body["model"] = model;
    body["max_tokens"] = 4096;
    body["system"] = sys_prompt;
    body["messages"] = nlohmann::json::array();
    for (const auto& m : messages) {
        nlohmann::json msg;
        msg["role"] = m.role;
        msg["content"] = m.content;
        body["messages"].push_back(std::move(msg));
    }
    return body.dump();
}

std::string DotGenerator::build_openai_body(const std::string& sys_prompt,
                                             const std::vector<ChatMessage>& messages,
                                             const std::string& model) const {
    nlohmann::json body;
    body["model"] = model;
    body["messages"] = nlohmann::json::array();

    // System message first
    nlohmann::json sys;
    sys["role"] = "system";
    sys["content"] = sys_prompt;
    body["messages"].push_back(std::move(sys));

    for (const auto& m : messages) {
        nlohmann::json msg;
        msg["role"] = m.role;
        msg["content"] = m.content;
        body["messages"].push_back(std::move(msg));
    }
    return body.dump();
}

std::string DotGenerator::build_google_body(const std::string& sys_prompt,
                                             const std::vector<ChatMessage>& messages) const {
    nlohmann::json body;

    // System instruction
    nlohmann::json sys_inst;
    nlohmann::json sys_part;
    sys_part["text"] = sys_prompt;
    sys_inst["parts"] = nlohmann::json::array({sys_part});
    body["system_instruction"] = sys_inst;

    body["contents"] = nlohmann::json::array();
    for (const auto& m : messages) {
        nlohmann::json content;
        std::string role = (m.role == "assistant") ? "model" : "user";
        content["role"] = role;
        nlohmann::json part;
        part["text"] = m.content;
        content["parts"] = nlohmann::json::array({part});
        body["contents"].push_back(std::move(content));
    }
    return body.dump();
}

Result<std::string> DotGenerator::http_post(
        const std::string& url,
        const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers) {
#ifdef NEEDLE_HAS_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        return Result<std::string>::failure("failed to init curl");
    }

    std::string response;
    struct curl_slist* hdr_list = nullptr;
    for (const auto& h : headers) {
        std::string line = h.first + ": " + h.second;
        hdr_list = curl_slist_append(hdr_list, line.c_str());
    }
    hdr_list = curl_slist_append(hdr_list, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdr_list);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return Result<std::string>::failure(std::string("curl error: ") + curl_easy_strerror(res));
    }

    if (http_code != 200) {
        return Result<std::string>::failure("HTTP " + std::to_string(http_code) + ": " + response);
    }

    return Result<std::string>::success(response);
#else
    (void)url; (void)body; (void)headers;
    return Result<std::string>::failure("libcurl not available — cannot call LLM");
#endif
}

Result<std::string> DotGenerator::call_llm(const std::string& provider,
                                            const std::string& sys_prompt,
                                            const std::vector<ChatMessage>& messages) {
    if (messages.empty()) {
        return Result<std::string>::failure("no messages provided");
    }

    auto info = get_provider(provider);

    // Resolve API key: env var > config file > empty
    std::string provider_key_name = provider;
    if (provider == "google") provider_key_name = "gemini";
    std::string api_key = NeedleConfig::global().resolve_api_key(provider_key_name);
    if (api_key.empty()) {
        return Result<std::string>::failure(
            std::string("API key not configured. Set ") + info.api_key_env +
            " or use: needle config set providers." + provider_key_name + ".api_key <key>");
    }

    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    if (provider == "anthropic") {
        url = std::string(info.base_url) + "/v1/messages";
        body = build_anthropic_body(sys_prompt, messages, info.default_model);
        headers.push_back({"x-api-key", api_key});
        headers.push_back({"anthropic-version", "2023-06-01"});
    } else if (provider == "openai") {
        url = std::string(info.base_url) + "/v1/chat/completions";
        body = build_openai_body(sys_prompt, messages, info.default_model);
        headers.push_back({"Authorization", "Bearer " + api_key});
    } else if (provider == "google") {
        url = std::string(info.base_url) + "/v1beta/models/" +
              info.default_model + ":generateContent";
        headers.push_back({"x-goog-api-key", api_key});
        body = build_google_body(sys_prompt, messages);
    } else {
        return Result<std::string>::failure("unknown provider: " + provider);
    }

    auto result = http_post(url, body, headers);
    if (!result.ok()) {
        return result;
    }

    // Parse response to extract text
    try {
        nlohmann::json j = nlohmann::json::parse(result.value());

        if (provider == "anthropic") {
            if (j.count("content") && j["content"].is_array() && !j["content"].empty()) {
                return Result<std::string>::success(
                    j["content"][0]["text"].get<std::string>());
            }
        } else if (provider == "openai") {
            if (j.count("choices") && j["choices"].is_array() && !j["choices"].empty()) {
                return Result<std::string>::success(
                    j["choices"][0]["message"]["content"].get<std::string>());
            }
        } else if (provider == "google") {
            if (j.count("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
                auto& content = j["candidates"][0]["content"];
                if (content.count("parts") && content["parts"].is_array() && !content["parts"].empty()) {
                    return Result<std::string>::success(
                        content["parts"][0]["text"].get<std::string>());
                }
            }
        }

        return Result<std::string>::failure("unexpected response format");
    } catch (const std::exception& e) {
        return Result<std::string>::failure(std::string("JSON parse error: ") + e.what());
    }
}

Result<std::string> DotGenerator::generate(const std::string& provider,
                                            const std::vector<ChatMessage>& messages) {
    return call_llm(provider, system_prompt(), messages);
}

Result<std::string> DotGenerator::chat(const std::string& sys_prompt,
                                        const std::vector<ChatMessage>& messages) {
    return call_llm("anthropic", sys_prompt, messages);
}

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
