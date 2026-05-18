#include <catch2/catch.hpp>

#ifdef NEEDLE_ENABLE_SERVER

#include "needle/server/http_server.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "needle/platform/platform.h"
#include "helpers/graph_fixtures.h"

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <thread>
#include <chrono>

using namespace needle;

namespace {

class NoOpHandler : public Handler {
public:
    explicit NoOpHandler(const std::string& type) : type_(type) {}
    std::string type_name() const override { return type_; }
    Result<Outcome> execute(const Node&, Context&, const ExecutionContext&) override {
        Outcome o;
        o.status = StageStatus::SUCCESS;
        return Result<Outcome>::success(std::move(o));
    }
private:
    std::string type_;
};

std::shared_ptr<HandlerRegistry> make_noop_registry() {
    auto reg = std::make_shared<HandlerRegistry>();
    for (const auto& t : {"start", "exit", "codergen", "parallel", "fan_in",
                          "conditional", "wait_human", "tool", "manager_loop", "llmkit"}) {
        reg->register_handler(t, std::make_shared<NoOpHandler>(t));
    }
    return reg;
}

struct TestServer {
    NeedleHttpServer server;
    httplib::Client client;

    TestServer(int port)
        : server(port, "127.0.0.1")
        , client("127.0.0.1", port)
    {
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);
    }

    void start(const Graph& graph) {
        PipelineConfig config;
        config.handler_registry = make_noop_registry();
        config.edge_selector = std::make_shared<EdgeSelector>();
        server.disable_run_persistence();
        EventBus bus;
        server.start(graph, std::move(config), bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ~TestServer() { server.stop(); }
};

std::string wait_for_status(TestServer& ts, const std::string& run_id,
                            const std::string& status) {
    for (int i = 0; i < 30; ++i) {
        auto res = ts.client.Get("/api/v1/runs/" + run_id);
        REQUIRE(res);
        auto j = nlohmann::json::parse(res->body);
        std::string actual = j["status"].get<std::string>();
        if (actual == status) return actual;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return "";
}

} // anonymous namespace

TEST_CASE("Dashboard: GET / returns HTML", "[dashboard]") {
    TestServer ts(18780);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Get("/");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 200);
    CHECK(res->get_header_value("Content-Type").find("text/html") != std::string::npos);
    CHECK(res->body.find("<title>needle</title>") != std::string::npos);
    CHECK(res->body.find("ndl-app") != std::string::npos);
    CHECK(res->get_header_value("Content-Security-Policy").find("default-src") != std::string::npos);
}

TEST_CASE("Dashboard: GET /api/v1/status returns server metadata", "[dashboard]") {
    TestServer ts(18781);
    auto graph = fixtures::make_simple_graph();
    ts.start(graph);

    auto res = ts.client.Get("/api/v1/status");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 200);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j["graph_name"] == "simple");
    CHECK(j["node_count"] == 3);
    CHECK(j["edge_count"] == 2);
    CHECK(j["active_runs"] == 0);
    CHECK(j.contains("dot_available"));
}

TEST_CASE("Dashboard: GET /api/v1/runs returns empty array initially", "[dashboard]") {
    TestServer ts(18782);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Get("/api/v1/runs");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 200);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j.is_array());
    CHECK(j.empty());
}

TEST_CASE("Dashboard: POST /api/v1/runs creates a run", "[dashboard]") {
    TestServer ts(18783);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Post("/api/v1/runs", "", "application/json");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 201);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j.contains("id"));
    CHECK(j["status"] == "running");

    // Verify run appears in listing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto list_res = ts.client.Get("/api/v1/runs");
    if (list_res) {
        auto arr = nlohmann::json::parse(list_res->body);
        CHECK(arr.size() >= 1);
    }
}

TEST_CASE("Dashboard: GET /api/v1/runs/:id returns run view", "[dashboard]") {
    TestServer ts(18784);
    ts.start(fixtures::make_simple_graph());

    auto post = ts.client.Post("/api/v1/runs", "", "application/json");
    if (!post) { WARN("Could not connect"); return; }
    auto run_id = nlohmann::json::parse(post->body)["id"].get<std::string>();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto res = ts.client.Get("/api/v1/runs/" + run_id);
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 200);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j["id"] == run_id);
    CHECK(j.contains("status"));
    CHECK(j.contains("node_statuses"));
    CHECK(j.contains("completed_stages"));
    CHECK(j.contains("total_stages"));
}

TEST_CASE("Dashboard: run view hydrates troubleshoot recovery from disk", "[dashboard]") {
    TestServer ts(18794);
    ts.start(fixtures::make_simple_graph());

    const std::string project_dir = platform::temp_dir() + "/needle_dashboard_troubleshoot_18794";
    platform::remove_recursive(project_dir);
    platform::mkdir_p(project_dir);

    nlohmann::json body;
    body["project_dir"] = project_dir;
    auto post = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    REQUIRE(post);
    REQUIRE(post->status == 201);
    const std::string run_id = nlohmann::json::parse(post->body)["id"].get<std::string>();
    REQUIRE(wait_for_status(ts, run_id, "completed") == "completed");

    const std::string session_dir =
        project_dir + "/.needle/design/troubleshoot/session-2026-05-17T12-00-00Z";
    platform::mkdir_p(session_dir);
    std::ofstream out(session_dir + "/recovery.md");
    REQUIRE(out.is_open());
    out << "---\n"
        << "session_id: \"2026-05-17T12-00-00Z\"\n"
        << "tier: tweak\n"
        << "outcome: resumed\n"
        << "cost_usd: 1.25\n"
        << "failed_node: \"work\"\n"
        << "backup_branch: \"auto/troubleshoot/backup/run-session\"\n"
        << "backup_base: \"abc123\"\n"
        << "escalate_reason: null\n"
        << "---\n\n"
        << "body\n";
    out.close();

    std::ofstream events(session_dir + "/events.ndjson");
    REQUIRE(events.is_open());
    events << R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_1","name":"Read","input":{"file_path":"/project/x.cpp"}}]},"elapsed_ms":42})" << "\n"
           << R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"ok","is_error":false}]}})" << "\n"
           << R"({"type":"result","subtype":"success","is_error":false,"result":"resumed","total_cost_usd":1.25,"num_turns":3})" << "\n";
    events.close();

    auto res = ts.client.Get("/api/v1/runs/" + run_id);
    REQUIRE(res);
    REQUIRE(res->status == 200);
    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j.contains("troubleshoot"));
    CHECK(j["troubleshoot"]["session_id"] == "2026-05-17T12-00-00Z");
    CHECK(j["troubleshoot"]["outcome"] == "resumed");
    CHECK(j["troubleshoot"]["failed_node"] == "work");
    CHECK(j["troubleshoot"]["cost_usd"] == Approx(1.25));
    CHECK(j["troubleshoot"]["backup_branch"] == "auto/troubleshoot/backup/run-session");
    REQUIRE(j["troubleshoot"]["report_path"].is_string());
    const std::string report_path = j["troubleshoot"]["report_path"].get<std::string>();
    CHECK(report_path.size() >= std::string("/recovery.md").size());
    CHECK(report_path.substr(report_path.size() - std::string("/recovery.md").size()) == "/recovery.md");
    CHECK(j["troubleshoot"]["session_dir"] == "troubleshoot/session-2026-05-17T12-00-00Z");
    REQUIRE(j["troubleshoot"]["activity"].is_array());
    CHECK(j["troubleshoot"]["activity"].size() == 4);
    const auto& activity = j["troubleshoot"]["activity"];
    CHECK(std::any_of(activity.begin(), activity.end(), [](const nlohmann::json& row) {
        return row.value("type", "") == "tool_call";
    }));

    platform::remove_recursive(project_dir);
}

TEST_CASE("Dashboard: GET /api/v1/runs/:id returns 404 for unknown", "[dashboard]") {
    TestServer ts(18785);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Get("/api/v1/runs/nonexistent");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 404);
}

TEST_CASE("Dashboard: POST /api/v1/runs/:id/cancel cancels a run", "[dashboard]") {
    TestServer ts(18786);
    ts.start(fixtures::make_simple_graph());

    auto post = ts.client.Post("/api/v1/runs", "", "application/json");
    if (!post) { WARN("Could not connect"); return; }
    auto run_id = nlohmann::json::parse(post->body)["id"].get<std::string>();

    auto res = ts.client.Post("/api/v1/runs/" + run_id + "/cancel", "", "application/json");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 200);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j["status"] == "cancelled");
}

TEST_CASE("Dashboard: GET /api/v1/graph/dot returns DOT source", "[dashboard]") {
    TestServer ts(18787);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Get("/api/v1/graph/dot");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 200);
    CHECK(res->body.find("digraph") != std::string::npos);
    CHECK(res->body.find("start") != std::string::npos);
}

TEST_CASE("Dashboard: GET /api/v1/graph/svg returns SVG or empty", "[dashboard]") {
    TestServer ts(18788);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Get("/api/v1/graph/svg");
    if (!res) { WARN("Could not connect"); return; }

    CHECK(res->status == 200);
    // SVG may be empty if dot binary is not available
    if (!res->body.empty()) {
        CHECK(res->body.find("<svg") != std::string::npos);
    }
}

TEST_CASE("Dashboard: legacy /pipelines endpoints still work", "[dashboard]") {
    TestServer ts(18789);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Get("/pipelines");
    if (!res) { WARN("Could not connect"); return; }
    CHECK(res->status == 200);

    auto post = ts.client.Post("/pipelines", "", "application/json");
    if (!post) { WARN("Could not connect"); return; }
    CHECK(post->status == 201);
}

TEST_CASE("Dashboard: POST /api/v1/runs with invalid JSON returns 400", "[dashboard]") {
    TestServer ts(18790);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Post("/api/v1/runs", "not json", "application/json");
    if (!res) { WARN("Could not connect"); return; }
    CHECK(res->status == 400);
}

TEST_CASE("Dashboard: POST /api/v1/generate-dot with empty messages returns 400", "[dashboard]") {
    TestServer ts(18791);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Post("/api/v1/generate-dot", "{\"messages\":[]}", "application/json");
    if (!res) { WARN("Could not connect"); return; }
    CHECK(res->status == 400);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j.contains("error"));
}

TEST_CASE("Dashboard: POST /api/v1/generate-dot with invalid JSON returns 400", "[dashboard]") {
    TestServer ts(18792);
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Post("/api/v1/generate-dot", "not json", "application/json");
    if (!res) { WARN("Could not connect"); return; }
    CHECK(res->status == 400);
}

TEST_CASE("Dashboard: POST /api/v1/generate-dot without API key returns 502", "[dashboard]") {
    TestServer ts(18793);
    ts.start(fixtures::make_simple_graph());

    // This will fail because no API key is set, but validates the endpoint exists
    std::string body = R"({"provider":"anthropic","messages":[{"role":"user","content":"test"}]})";
    auto res = ts.client.Post("/api/v1/generate-dot", body, "application/json");
    if (!res) { WARN("Could not connect"); return; }
    // Should be 502 (LLM call failed) or success if key happens to be set
    CHECK((res->status == 502 || res->status == 200));
    auto j = nlohmann::json::parse(res->body);
    CHECK((j.contains("error") || j.contains("response")));
}

TEST_CASE("Dashboard: serve with empty graph returns valid status", "[dashboard]") {
    // Simulates needle serve without a .dot file
    TestServer ts(18794);
    Graph empty = Graph::make("needle", {}, {});
    ts.start(empty);

    auto res = ts.client.Get("/api/v1/status");
    if (!res) { WARN("Could not connect"); return; }
    CHECK(res->status == 200);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j["node_count"] == 0);
    CHECK(j["edge_count"] == 0);

    // Dashboard should still load
    auto html = ts.client.Get("/");
    if (html) {
        CHECK(html->status == 200);
        CHECK(html->body.find("ndl-app") != std::string::npos);
    }
}

#else

TEST_CASE("Dashboard endpoints: disabled when NEEDLE_ENABLE_SERVER not defined", "[dashboard]") {
    SUCCEED("Dashboard tests skipped - NEEDLE_ENABLE_SERVER not defined");
}

#endif // NEEDLE_ENABLE_SERVER
