#include <catch2/catch.hpp>
#include "needle/handlers/all_handlers.h"
#include "needle/backend/backend.h"
#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/event/event_bus.h"
#include "needle/util/fs_helpers.h"
#include <atomic>
#include <fstream>
#include "needle/platform/platform.h"

using namespace needle;

namespace {

class MockBackend : public Backend {
public:
    Outcome configured_outcome;
    std::string status_json_to_write;  // If non-empty, write to stage_dir/status.json

    MockBackend() {
        configured_outcome.status = StageStatus::SUCCESS;
        configured_outcome.output = "mock output";
    }

    std::string name() const override { return "mock"; }

    Result<Outcome> execute(const Node& /*node*/, Context& /*ctx*/,
                            const std::string& stage_dir) override {
        // Simulate agent writing status.json during execution
        if (!status_json_to_write.empty() && !stage_dir.empty()) {
            needle::mkdir_p(stage_dir);
            std::ofstream out(stage_dir + "/status.json");
            if (out.is_open()) out << status_json_to_write;
        }
        return Result<Outcome>::success(configured_outcome);
    }
};

void rmdir_r(const std::string& path) {
    std::remove((path + "/status.json").c_str());
    std::remove((path + "/prompt.md").c_str());
    std::remove((path + "/response.md").c_str());
    std::remove((path + "/test_node/status.json").c_str());
    platform::remove_dir(path + "/test_node");
    platform::remove_dir(path);
}

} // anonymous namespace

TEST_CASE("CodergenHandler: success with mock backend", "[codergen_handler]") {
    auto mock_backend = std::make_shared<MockBackend>();
    auto handler = make_codergen_handler(mock_backend);

    Node node;
    node.id = "test_node";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "write code");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::SUCCESS);
    REQUIRE(result.value().output == "mock output");
}

TEST_CASE("CodergenHandler: failure from backend", "[codergen_handler]") {
    auto mock_backend = std::make_shared<MockBackend>();
    mock_backend->configured_outcome.status = StageStatus::FAILURE;
    mock_backend->configured_outcome.output = "compilation error";

    auto handler = make_codergen_handler(mock_backend);

    Node node;
    node.id = "test_node";
    node.type = NodeType::CODERGEN;

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root;
    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::FULL, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    REQUIRE(result.value().status == StageStatus::FAILURE);
}

TEST_CASE("CodergenHandler: exit code 0 always returns SUCCESS", "[codergen_handler]") {
    // Verify that even if an agent writes a status.json with FAILURE,
    // the handler trusts the backend's exit code (0 = success)
    auto mock_backend = std::make_shared<MockBackend>();
    auto handler = make_codergen_handler(mock_backend);

    Node node;
    node.id = "test_node";
    node.type = NodeType::CODERGEN;
    node.attrs.set("prompt", "write code");

    Context ctx;
    EventBus bus;
    std::atomic<bool> cancelled(false);
    std::string logs_root = platform::temp_dir() + "/needle_test_codergen";

    // Even if mock writes a FAILURE status.json, handler ignores it
    mock_backend->status_json_to_write =
        R"({"status":"FAILURE","output":"agent thinks it failed"})";

    Graph graph = Graph::make("test", {node}, {});
    ExecutionContext exec_ctx{graph, bus, logs_root, logs_root, FidelityMode::COMPACT, cancelled};

    auto result = handler->execute(node, ctx, exec_ctx);
    REQUIRE(result.ok());
    // Backend returned SUCCESS (exit_code=0), so outcome is SUCCESS
    // regardless of what the agent wrote to status.json
    REQUIRE(result.value().status == StageStatus::SUCCESS);

    rmdir_r(logs_root);
}
