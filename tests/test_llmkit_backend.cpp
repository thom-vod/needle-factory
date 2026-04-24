#include <catch2/catch.hpp>
#include "needle/backend/llmkit_backend.h"
#include <nlohmann/json.hpp>

using namespace needle;

namespace {

std::map<std::string, ProviderConfig> make_test_providers() {
    std::map<std::string, ProviderConfig> providers;

    ProviderConfig anthropic;
    anthropic.name = "anthropic";
    anthropic.base_url = "https://api.anthropic.com";
    anthropic.api_key_env = "ANTHROPIC_API_KEY";
    anthropic.default_model = "claude-sonnet-4-20250514";
    providers["anthropic"] = anthropic;

    ProviderConfig openai;
    openai.name = "openai";
    openai.base_url = "https://api.openai.com";
    openai.api_key_env = "OPENAI_API_KEY";
    openai.default_model = "gpt-4";
    providers["openai"] = openai;

    ProviderConfig google;
    google.name = "google";
    google.base_url = "https://generativelanguage.googleapis.com";
    google.api_key_env = "GOOGLE_API_KEY";
    google.default_model = "gemini-2.0-flash";
    providers["google"] = google;

    return providers;
}

} // anonymous namespace

TEST_CASE("LLMKitBackend: build_anthropic_request produces valid JSON", "[llmkit]") {
    auto providers = make_test_providers();
    LLMKitBackend backend(providers);

    std::string body = backend.build_anthropic_request("hello world", "claude-sonnet-4-20250514");

    auto j = nlohmann::json::parse(body);
    REQUIRE(j["model"] == "claude-sonnet-4-20250514");
    REQUIRE(j["messages"].is_array());
    REQUIRE(j["messages"].size() == 1);
    REQUIRE(j["messages"][0]["role"] == "user");
    REQUIRE(j["messages"][0]["content"] == "hello world");
    REQUIRE(j["max_tokens"] == 4096);
}

TEST_CASE("LLMKitBackend: build_openai_request produces valid JSON", "[llmkit]") {
    auto providers = make_test_providers();
    LLMKitBackend backend(providers);

    std::string body = backend.build_openai_request("test prompt", "gpt-4");

    auto j = nlohmann::json::parse(body);
    REQUIRE(j["model"] == "gpt-4");
    REQUIRE(j["messages"].is_array());
    REQUIRE(j["messages"].size() == 1);
    REQUIRE(j["messages"][0]["role"] == "user");
    REQUIRE(j["messages"][0]["content"] == "test prompt");
}

TEST_CASE("LLMKitBackend: build_google_request produces valid JSON", "[llmkit]") {
    auto providers = make_test_providers();
    LLMKitBackend backend(providers);

    std::string body = backend.build_google_request("what is AI?");

    auto j = nlohmann::json::parse(body);
    REQUIRE(j["contents"].is_array());
    REQUIRE(j["contents"].size() == 1);
    REQUIRE(j["contents"][0]["parts"].is_array());
    REQUIRE(j["contents"][0]["parts"].size() == 1);
    REQUIRE(j["contents"][0]["parts"][0]["text"] == "what is AI?");
}

TEST_CASE("LLMKitBackend: name returns llmkit", "[llmkit]") {
    auto providers = make_test_providers();
    LLMKitBackend backend(providers);
    REQUIRE(backend.name() == "llmkit");
}

TEST_CASE("LLMKitBackend: unknown provider returns failure", "[llmkit]") {
    auto providers = make_test_providers();
    LLMKitBackend backend(providers);

    Node node;
    node.id = "test";
    node.type = NodeType::LLMKIT;
    node.attrs.set("llm_provider", "nonexistent");
    node.attrs.set("prompt", "hello");

    Context ctx;
    auto result = backend.execute(node, ctx, "");
    REQUIRE_FALSE(result.ok());
}
