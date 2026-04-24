#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/backend/process_runner.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include <atomic>
#include <cstdlib>

#ifdef _WIN32
static inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#endif

using namespace needle;

TEST_CASE("WebSearchHandler: type_name is web_search", "[web_search_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_web_search_handler(runner);
    REQUIRE(handler->type_name() == "web_search");
}

TEST_CASE("WebSearchHandler: falls back to label when no query", "[web_search_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_web_search_handler(runner);

    Node node;
    node.id = "search1";
    node.type = NodeType::CODERGEN;
    // No query attribute, but label() falls back to node.id

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    // Without API key, should gracefully degrade
    const char* old_key = getenv("TAVILY_API_KEY");
    if (old_key) unsetenv("TAVILY_API_KEY");

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().context_updates["web_search.search1.skipped"] == "true");

    if (old_key) setenv("TAVILY_API_KEY", old_key, 1);
}

TEST_CASE("WebSearchHandler: graceful degradation without API key", "[web_search_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_web_search_handler(runner);

    Node node;
    node.id = "search2";
    node.type = NodeType::CODERGEN;
    node.attrs.set("query", "test search");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    // Temporarily unset TAVILY_API_KEY
    const char* old_key = getenv("TAVILY_API_KEY");
    if (old_key) unsetenv("TAVILY_API_KEY");

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().context_updates.count("web_search.search2.skipped"));
    REQUIRE(result.value().context_updates["web_search.search2.skipped"] == "true");

    // Restore
    if (old_key) setenv("TAVILY_API_KEY", old_key, 1);
}

TEST_CASE("WebSearchHandler: parses Tavily response", "[web_search_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();

    // Mock a successful Tavily response
    ProcessResult resp;
    resp.exit_code = 0;
    resp.timed_out = false;
    resp.stdout_output = R"({
        "answer": "Test answer summary",
        "results": [
            {"title": "Result 1", "url": "https://example.com/1", "content": "First result content"},
            {"title": "Result 2", "url": "https://example.com/2", "content": "Second result content"}
        ]
    })";
    runner->enqueue(resp);

    auto handler = make_web_search_handler(runner);

    Node node;
    node.id = "search3";
    node.type = NodeType::CODERGEN;
    node.attrs.set("query", "test query");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    // Set API key so handler attempts the search
    setenv("TAVILY_API_KEY", "test-key", 1);

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);

    // Verify context updates
    REQUIRE(result.value().context_updates.count("web_search.search3.results"));
    REQUIRE(result.value().context_updates.count("web_search.search3.count"));
    REQUIRE(result.value().context_updates["web_search.search3.count"] == "2");

    // Verify markdown contains results
    auto results = result.value().context_updates["web_search.search3.results"];
    REQUIRE(results.find("Test answer summary") != std::string::npos);
    REQUIRE(results.find("Result 1") != std::string::npos);
    REQUIRE(results.find("Result 2") != std::string::npos);

    unsetenv("TAVILY_API_KEY");
}

TEST_CASE("WebSearchHandler: curl failure degrades gracefully", "[web_search_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 7; // curl connection error
    resp.timed_out = false;
    resp.stdout_output = "";
    resp.stderr_output = "curl: (7) Failed to connect";
    runner->enqueue(resp);

    auto handler = make_web_search_handler(runner);

    Node node;
    node.id = "search4";
    node.type = NodeType::CODERGEN;
    node.attrs.set("query", "test query");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    setenv("TAVILY_API_KEY", "test-key", 1);

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().context_updates["web_search.search4.skipped"] == "true");

    unsetenv("TAVILY_API_KEY");
}
