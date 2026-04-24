#include "needle/backend/llmkit_backend.h"
#include "needle/config/needle_config.h"
#include "needle/util/fs_helpers.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include "needle/platform/platform.h"

#ifdef NEEDLE_HAS_CURL
#include <curl/curl.h>
#endif

namespace needle {

namespace {

#ifdef NEEDLE_HAS_CURL
static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* response = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    response->append(ptr, total);
    return total;
}
#endif

} // anonymous namespace

LLMKitBackend::LLMKitBackend(std::map<std::string, ProviderConfig> providers)
    : providers_(std::move(providers))
{
}

std::string LLMKitBackend::name() const {
    return "llmkit";
}

std::string LLMKitBackend::build_anthropic_request(const std::string& prompt,
                                                     const std::string& model) const {
    nlohmann::json body;
    body["model"] = model;
    body["max_tokens"] = 4096;
    body["messages"] = nlohmann::json::array();
    nlohmann::json msg;
    msg["role"] = "user";
    msg["content"] = prompt;
    body["messages"].push_back(std::move(msg));
    return body.dump();
}

std::string LLMKitBackend::build_openai_request(const std::string& prompt,
                                                   const std::string& model) const {
    nlohmann::json body;
    body["model"] = model;
    body["messages"] = nlohmann::json::array();
    nlohmann::json msg;
    msg["role"] = "user";
    msg["content"] = prompt;
    body["messages"].push_back(std::move(msg));
    return body.dump();
}

std::string LLMKitBackend::build_google_request(const std::string& prompt) const {
    nlohmann::json body;
    body["contents"] = nlohmann::json::array();
    nlohmann::json content;
    content["parts"] = nlohmann::json::array();
    nlohmann::json part;
    part["text"] = prompt;
    content["parts"].push_back(std::move(part));
    body["contents"].push_back(std::move(content));
    return body.dump();
}

StageStatus LLMKitBackend::classify_http_error(int status_code) {
    if (status_code == 429 || (status_code >= 500 && status_code < 600)) {
        return StageStatus::RETRY;
    }
    return StageStatus::FAILURE;
}

Result<std::string> LLMKitBackend::call_anthropic(const std::string& prompt,
                                                    const std::string& model,
                                                    const std::string& api_key) {
#ifdef NEEDLE_HAS_CURL
    auto it = providers_.find("anthropic");
    if (it == providers_.end()) {
        return Result<std::string>::failure("anthropic provider not configured");
    }

    std::string url = it->second.base_url + "/v1/messages";
    std::string body = build_anthropic_request(prompt, model);

    CURL* curl = curl_easy_init();
    if (!curl) {
        return Result<std::string>::failure("failed to init curl");
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("x-api-key: " + api_key).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return Result<std::string>::failure(std::string("curl error: ") + curl_easy_strerror(res));
    }

    if (http_code != 200) {
        return Result<std::string>::failure("HTTP " + std::to_string(http_code) + ": " + response);
    }

    // Parse response
    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.count("content") && j["content"].is_array() && !j["content"].empty()) {
            return Result<std::string>::success(j["content"][0]["text"].get<std::string>());
        }
        return Result<std::string>::success(response);
    } catch (const std::exception& e) {
        return Result<std::string>::failure(std::string("JSON parse error: ") + e.what());
    }
#else
    (void)prompt; (void)model; (void)api_key;
    return Result<std::string>::failure("libcurl not available");
#endif
}

Result<std::string> LLMKitBackend::call_openai(const std::string& prompt,
                                                 const std::string& model,
                                                 const std::string& api_key) {
#ifdef NEEDLE_HAS_CURL
    auto it = providers_.find("openai");
    if (it == providers_.end()) {
        return Result<std::string>::failure("openai provider not configured");
    }

    std::string url = it->second.base_url + "/v1/chat/completions";
    std::string body = build_openai_request(prompt, model);

    CURL* curl = curl_easy_init();
    if (!curl) {
        return Result<std::string>::failure("failed to init curl");
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return Result<std::string>::failure(std::string("curl error: ") + curl_easy_strerror(res));
    }

    if (http_code != 200) {
        return Result<std::string>::failure("HTTP " + std::to_string(http_code) + ": " + response);
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.count("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            return Result<std::string>::success(
                j["choices"][0]["message"]["content"].get<std::string>());
        }
        return Result<std::string>::success(response);
    } catch (const std::exception& e) {
        return Result<std::string>::failure(std::string("JSON parse error: ") + e.what());
    }
#else
    (void)prompt; (void)model; (void)api_key;
    return Result<std::string>::failure("libcurl not available");
#endif
}

Result<std::string> LLMKitBackend::call_google(const std::string& prompt,
                                                 const std::string& model,
                                                 const std::string& api_key) {
#ifdef NEEDLE_HAS_CURL
    auto it = providers_.find("google");
    if (it == providers_.end()) {
        return Result<std::string>::failure("google provider not configured");
    }

    std::string url = it->second.base_url + "/v1beta/models/" + model +
                      ":generateContent";
    std::string body = build_google_request(prompt);

    CURL* curl = curl_easy_init();
    if (!curl) {
        return Result<std::string>::failure("failed to init curl");
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string gemini_auth = "x-goog-api-key: " + api_key;
    headers = curl_slist_append(headers, gemini_auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return Result<std::string>::failure(std::string("curl error: ") + curl_easy_strerror(res));
    }

    if (http_code != 200) {
        return Result<std::string>::failure("HTTP " + std::to_string(http_code) + ": " + response);
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.count("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
            auto& content = j["candidates"][0]["content"];
            if (content.count("parts") && content["parts"].is_array() && !content["parts"].empty()) {
                return Result<std::string>::success(
                    content["parts"][0]["text"].get<std::string>());
            }
        }
        return Result<std::string>::success(response);
    } catch (const std::exception& e) {
        return Result<std::string>::failure(std::string("JSON parse error: ") + e.what());
    }
#else
    (void)prompt; (void)model; (void)api_key;
    return Result<std::string>::failure("libcurl not available");
#endif
}

Result<Outcome> LLMKitBackend::execute(const Node& node, Context& ctx,
                                        const std::string& stage_dir) {
    (void)ctx;

    std::string prompt = node.prompt();
    if (prompt.empty()) {
        prompt = node.label();
    }

    // Resolve provider
    std::string provider_name = node.attrs.get("llm_provider", "anthropic");

    auto pit = providers_.find(provider_name);
    if (pit == providers_.end()) {
        return Result<Outcome>::failure("unknown provider: " + provider_name);
    }

    const ProviderConfig& pconf = pit->second;

    // Get model
    std::string model = node.attrs.get("llm_model");
    if (model.empty()) {
        model = pconf.default_model;
    }

    // Get API key from config (env var takes precedence, then config file)
    std::string api_key = NeedleConfig::global().resolve_api_key(provider_name);
    if (api_key.empty()) {
        return Result<Outcome>::failure(
            "API key not configured for " + provider_name +
            ". Set " + pconf.api_key_env + " or use: needle config set providers." +
            provider_name + ".api_key <key>");
    }

    // Write prompt
    if (!stage_dir.empty()) {
        platform::mkdir_p(stage_dir);
        std::ofstream out(stage_dir + "/prompt.md");
        if (out.is_open()) {
            out << prompt;
        }
    }

    // Call provider
    Result<std::string> call_result = Result<std::string>::failure("unknown provider");
    if (provider_name == "anthropic") {
        call_result = call_anthropic(prompt, model, api_key);
    } else if (provider_name == "openai") {
        call_result = call_openai(prompt, model, api_key);
    } else if (provider_name == "google") {
        call_result = call_google(prompt, model, api_key);
    } else {
        return Result<Outcome>::failure("unsupported provider: " + provider_name);
    }

    Outcome outcome;
    if (call_result.ok()) {
        outcome.status = StageStatus::SUCCESS;
        outcome.output = call_result.value();

        // Write response
        if (!stage_dir.empty()) {
            std::ofstream out(stage_dir + "/response.md");
            if (out.is_open()) {
                out << outcome.output;
            }
        }
    } else {
        // Try to classify the error
        outcome.status = StageStatus::FAILURE;
        outcome.output = call_result.error();
    }

    return Result<Outcome>::success(std::move(outcome));
}

} // namespace needle
