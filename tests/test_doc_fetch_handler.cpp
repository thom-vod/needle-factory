#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/backend/process_runner.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include <atomic>

using namespace needle;

TEST_CASE("DocFetchHandler: type_name is doc_fetch", "[doc_fetch_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_doc_fetch_handler(runner);
    REQUIRE(handler->type_name() == "doc_fetch");
}

TEST_CASE("DocFetchHandler: missing url skips gracefully", "[doc_fetch_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();
    auto handler = make_doc_fetch_handler(runner);

    Node node;
    node.id = "fetch1";
    node.type = NodeType::CODERGEN;

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().context_updates["doc_fetch.fetch1.skipped"] == "true");
}

TEST_CASE("DocFetchHandler: auto-detects git fetch type", "[doc_fetch_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();

    // Mock git clone failure (graceful degradation)
    ProcessResult resp;
    resp.exit_code = 128;
    resp.timed_out = false;
    resp.stdout_output = "";
    resp.stderr_output = "fatal: repository not found";
    runner->enqueue(resp);

    auto handler = make_doc_fetch_handler(runner);

    Node node;
    node.id = "fetch2";
    node.type = NodeType::CODERGEN;
    node.attrs.set("url", "https://github.com/example/repo.git");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().context_updates["doc_fetch.fetch2.skipped"] == "true");

    // Verify git was called (not curl)
    auto calls = runner->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].command == "git");
}

TEST_CASE("DocFetchHandler: web fetch with curl", "[doc_fetch_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();

    ProcessResult resp;
    resp.exit_code = 0;
    resp.timed_out = false;
    resp.stdout_output = "<html><body><h1>Hello</h1><p>World</p></body></html>";
    runner->enqueue(resp);

    auto handler = make_doc_fetch_handler(runner);

    Node node;
    node.id = "fetch3";
    node.type = NodeType::CODERGEN;
    node.attrs.set("url", "https://example.com/page");
    node.attrs.set("fetch_type", "web");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);

    auto content = result.value().context_updates["doc_fetch.fetch3.content"];
    REQUIRE(content.find("Hello") != std::string::npos);
    REQUIRE(content.find("World") != std::string::npos);
    // HTML tags should be stripped
    REQUIRE(content.find("<html>") == std::string::npos);
}

TEST_CASE("DocFetchHandler: pdf without pdftotext degrades gracefully", "[doc_fetch_handler]") {
    auto runner = std::make_shared<MockProcessRunner>();

    // curl download succeeds
    ProcessResult dl_resp;
    dl_resp.exit_code = 0;
    dl_resp.timed_out = false;
    runner->enqueue(dl_resp);

    // pdftotext fails (not installed)
    ProcessResult pdf_resp;
    pdf_resp.exit_code = 127;
    pdf_resp.timed_out = false;
    pdf_resp.stderr_output = "pdftotext: command not found";
    runner->enqueue(pdf_resp);

    auto handler = make_doc_fetch_handler(runner);

    Node node;
    node.id = "fetch4";
    node.type = NodeType::CODERGEN;
    node.attrs.set("url", "https://example.com/paper.pdf");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().context_updates["doc_fetch.fetch4.skipped"] == "true");
}
