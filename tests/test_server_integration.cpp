#include <catch2/catch.hpp>

#ifdef NEEDLE_ENABLE_SERVER

#include "needle/server/http_server.h"
#include "needle/config/needle_config.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/platform/platform.h"
#include "needle/util/fs_helpers.h"
#include "helpers/graph_fixtures.h"

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

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

class FailHandler : public Handler {
public:
    explicit FailHandler(const std::string& type) : type_(type) {}
    std::string type_name() const override { return type_; }
    Result<Outcome> execute(const Node&, Context&, const ExecutionContext&) override {
        Outcome o;
        o.status = StageStatus::FAILURE;
        o.output = "intentional test failure";
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

std::shared_ptr<HandlerRegistry> make_fail_tool_registry() {
    auto reg = std::make_shared<HandlerRegistry>();
    for (const auto& t : {"start", "exit", "codergen", "parallel", "fan_in",
                          "conditional", "wait_human", "manager_loop", "llmkit"}) {
        reg->register_handler(t, std::make_shared<NoOpHandler>(t));
    }
    // Tool handler fails
    reg->register_handler("tool", std::make_shared<FailHandler>("tool"));
    return reg;
}

struct IntegrationTestServer {
    NeedleHttpServer server;
    httplib::Client client;
    PipelineConfig config;

    IntegrationTestServer(int port, std::shared_ptr<HandlerRegistry> registry = nullptr)
        : server(port, "127.0.0.1")
        , client("127.0.0.1", port)
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
        config.handler_registry = registry ? registry : make_noop_registry();
        config.edge_selector = std::make_shared<EdgeSelector>();
        config.checkpoint_writer = std::make_shared<JsonCheckpointWriter>();
    }

    void start(const Graph& graph) {
        server.disable_run_persistence();
        EventBus bus;
        server.start(graph, config, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    ~IntegrationTestServer() { server.stop(); }
};

// Create a unique temp directory and return its path
std::string make_temp_dir(const std::string& suffix) {
    std::string path = "/tmp/needle_test_" + suffix + "_" +
                       std::to_string(std::rand() % 100000);
    needle::mkdir_p(path);
    return path;
}

// Recursively remove a directory
void remove_dir(const std::string& path) {
    std::string cmd = "rm -rf '" + path + "'";
    int rc = std::system(cmd.c_str());
    (void)rc;
}

std::string read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string wait_for_recovery_report(const std::string& project_dir,
                                     const std::string& stem,
                                     const std::string& expected_tier,
                                     int timeout_ms = 5000) {
    const std::string base = project_dir + "/.needle/" + stem + "/troubleshoot";
    int elapsed = 0;
    const int poll_interval = 100;
    while (elapsed < timeout_ms) {
        if (platform::is_directory(base)) {
            for (const auto& entry : platform::list_directory(base)) {
                const std::string report_path = base + "/" + entry + "/recovery.md";
                const std::string report = read_text(report_path);
                if (report.find("tier: " + expected_tier) != std::string::npos) {
                    return report;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval));
        elapsed += poll_interval;
    }
    return "";
}

struct TroubleshootConfigRestore {
    std::string agent;
    std::string model;
    std::string mode;

    TroubleshootConfigRestore()
        : agent(NeedleConfig::global().get_string("defaults.troubleshoot_agent"))
        , model(NeedleConfig::global().get_string("defaults.troubleshoot_model"))
        , mode(NeedleConfig::global().get_string("defaults.troubleshoot_mode")) {}

    ~TroubleshootConfigRestore() {
        NeedleConfig::global().set("defaults.troubleshoot_agent", agent);
        NeedleConfig::global().set("defaults.troubleshoot_model", model);
        NeedleConfig::global().set("defaults.troubleshoot_mode", mode);
    }
};

// Poll a run status until it reaches a terminal state or timeout
nlohmann::json wait_for_run(httplib::Client& client, const std::string& run_id,
                            int timeout_ms = 5000) {
    int elapsed = 0;
    int poll_interval = 100;
    while (elapsed < timeout_ms) {
        auto res = client.Get("/api/v1/runs/" + run_id);
        if (res && res->status == 200) {
            auto j = nlohmann::json::parse(res->body);
            std::string status = j["status"].get<std::string>();
            if (status == "completed" || status == "failed" || status == "cancelled") {
                return j;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval));
        elapsed += poll_interval;
    }
    return nlohmann::json::object();
}

const char* SIMPLE_DOT_SOURCE =
    "digraph test {\n"
    "    graph [goal=\"test\", label=\"Test\"]\n"
    "    start [shape=Mdiamond, label=\"Start\"]\n"
    "    step [label=\"Step\", handler=\"tool\", command=\"echo hello\"]\n"
    "    exit [shape=Msquare, label=\"Done\"]\n"
    "    start -> step -> exit\n"
    "}\n";

const char* FAILING_DOT_SOURCE =
    "digraph test_fail {\n"
    "    graph [goal=\"test failure\", label=\"Test Fail\"]\n"
    "    start [shape=Mdiamond, label=\"Start\"]\n"
    "    step [label=\"Fail Step\", handler=\"tool\", command=\"false\"]\n"
    "    exit [shape=Msquare, label=\"Done\"]\n"
    "    start -> step -> exit\n"
    "}\n";

#ifndef _WIN32
const char* FAILING_TROUBLESHOOT_OFF_DOT_SOURCE =
    "digraph test_fail {\n"
    "    graph [goal=\"test failure\", label=\"Troubleshoot Override\", troubleshoot_on_failure=\"off\"]\n"
    "    start [shape=Mdiamond, label=\"Start\"]\n"
    "    step [label=\"Fail Step\", handler=\"tool\", command=\"false\"]\n"
    "    exit [shape=Msquare, label=\"Done\"]\n"
    "    start -> step -> exit\n"
    "}\n";
#endif

const char* FAILING_TROUBLESHOOT_DIAGNOSE_DOT_SOURCE =
    "digraph test_fail {\n"
    "    graph [goal=\"test failure\", label=\"Troubleshoot Graph\", troubleshoot_on_failure=\"diagnose\"]\n"
    "    start [shape=Mdiamond, label=\"Start\"]\n"
    "    step [label=\"Fail Step\", handler=\"tool\", command=\"false\"]\n"
    "    exit [shape=Msquare, label=\"Done\"]\n"
    "    start -> step -> exit\n"
    "}\n";

} // anonymous namespace

// ─── Template loading ─────────────────────────────────────────────────

TEST_CASE("ServerIntegration: GET /api/v1/templates returns template list", "[integration][server]") {
    IntegrationTestServer ts(18850);
    // Set up resource locator to find sample_dots directory
#ifdef NEEDLE_SOURCE_DIR
    ts.config.resource_locator = ResourceLocator(std::string(NEEDLE_SOURCE_DIR) + "/build");
#endif
    ts.start(fixtures::make_simple_graph());

    auto res = ts.client.Get("/api/v1/templates");
    if (!res) { WARN("Could not connect to test server"); return; }

    CHECK(res->status == 200);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j.is_array());

    // interactive_chat.dot has template="true", so it should appear
    bool found_template = false;
    for (const auto& t : j) {
        if (t.contains("name") && t["name"].get<std::string>() == "interactive_chat") {
            found_template = true;
            CHECK(t.contains("label"));
            CHECK(t.contains("params"));
        }
    }
    CHECK(found_template);
}

// ─── Run creation with DOT source ────────────────────────────────────

TEST_CASE("ServerIntegration: POST /api/v1/runs creates a run from DOT source", "[integration][server]") {
    IntegrationTestServer ts(18851);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("run_create");

    nlohmann::json body;
    body["dot_source"] = SIMPLE_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }

    CHECK(res->status == 201);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j.contains("id"));
    CHECK(j["status"] == "running");

    remove_dir(temp_dir);
}

TEST_CASE("ServerIntegration: POST /api/v1/runs rejects invalid troubleshoot mode",
          "[integration][server]") {
    IntegrationTestServer ts(18890);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("run_bad_troubleshoot_mode");

    nlohmann::json body;
    body["dot_source"] = SIMPLE_DOT_SOURCE;
    body["project_dir"] = temp_dir;
    body["troubleshoot_mode"] = "bogus";

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }

    REQUIRE(res->status == 400);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j["error"] == "invalid troubleshoot_mode");
    CHECK(j["got"] == "bogus");
    REQUIRE(j["allowed"].is_array());
    CHECK(j["allowed"] == nlohmann::json::array({"off", "diagnose", "tweak", "full"}));

    remove_dir(temp_dir);
}

TEST_CASE("ServerIntegration: POST /api/v1/runs uses graph troubleshoot mode without override",
          "[integration][server]") {
    TroubleshootConfigRestore restore;
    NeedleConfig::global().set("defaults.troubleshoot_agent", "/bin/echo");
    NeedleConfig::global().set("defaults.troubleshoot_model", "test-model");
    NeedleConfig::global().set("defaults.troubleshoot_mode", "off");

    IntegrationTestServer ts(18891, make_fail_tool_registry());
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("run_graph_troubleshoot_mode");

    nlohmann::json body;
    body["dot_source"] = FAILING_TROUBLESHOOT_DIAGNOSE_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();
    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());
    CHECK(run_view["status"] == "failed");

    const std::string report = wait_for_recovery_report(
        temp_dir, "troubleshoot_graph", "diagnose");
    REQUIRE_FALSE(report.empty());

    remove_dir(temp_dir);
}

TEST_CASE("ServerIntegration: POST /api/v1/runs troubleshoot mode overrides graph",
          "[integration][server]") {
#ifdef _WIN32
    SUCCEED("skipped on Windows");
#else
    TroubleshootConfigRestore restore;
    NeedleConfig::global().set("defaults.troubleshoot_agent", "/bin/echo");
    NeedleConfig::global().set("defaults.troubleshoot_model", "test-model");
    NeedleConfig::global().set("defaults.troubleshoot_mode", "off");

    IntegrationTestServer ts(18892, make_fail_tool_registry());
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("run_override_troubleshoot_mode");
    std::ofstream dot(temp_dir + "/flow.dot");
    REQUIRE(dot.is_open());
    dot << FAILING_TROUBLESHOOT_OFF_DOT_SOURCE;
    dot.close();
    REQUIRE(std::system(("cd '" + temp_dir + "' && git init -q && "
                         "git config user.email needle-test@example.com && "
                         "git config user.name 'Needle Test' && "
                         "git config commit.gpgsign false && "
                         "git add flow.dot && git commit -qm initial").c_str()) == 0);

    nlohmann::json body;
    body["dot_source"] = FAILING_TROUBLESHOOT_OFF_DOT_SOURCE;
    body["project_dir"] = temp_dir;
    body["troubleshoot_mode"] = "tweak";

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();
    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());
    CHECK(run_view["status"] == "failed");

    const std::string report = wait_for_recovery_report(
        temp_dir, "troubleshoot_override", "tweak");
    REQUIRE_FALSE(report.empty());

    remove_dir(temp_dir);
#endif
}

// ─── Run completion ──────────────────────────────────────────────────

TEST_CASE("ServerIntegration: run completes after DOT submission", "[integration][server]") {
    IntegrationTestServer ts(18852);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("run_complete");

    nlohmann::json body;
    body["dot_source"] = SIMPLE_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();

    auto run_view = wait_for_run(ts.client, run_id);
    CHECK_FALSE(run_view.empty());
    if (!run_view.empty()) {
        CHECK(run_view["status"] == "completed");
    }

    remove_dir(temp_dir);
}

// ─── Stage files written ─────────────────────────────────────────────

TEST_CASE("ServerIntegration: stage directories and status files are created", "[integration][server]") {
    IntegrationTestServer ts(18853);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("stage_files");

    nlohmann::json body;
    body["dot_source"] = SIMPLE_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();

    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());
    REQUIRE(run_view["status"] == "completed");

    // Verify the .needle directory structure was created (per-DOT subdirectory)
    std::string needle_dir = temp_dir + "/.needle";
    CHECK(needle::is_directory(needle_dir));

    // Stage directory uses per-DOT subdirectory (label="Test" → stem="test")
    std::string step_dir = needle_dir + "/test/stages/step";
    CHECK(needle::is_directory(step_dir));
    CHECK(needle::is_file(step_dir + "/status.json"));

    remove_dir(temp_dir);
}

// ─── Checkpoint written ──────────────────────────────────────────────

TEST_CASE("ServerIntegration: checkpoint.json is written on completed run", "[integration][server]") {
    IntegrationTestServer ts(18854);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("checkpoint");

    nlohmann::json body;
    body["dot_source"] = SIMPLE_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();

    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());
    REQUIRE(run_view["status"] == "completed");

    // Checkpoint uses per-DOT subdirectory (label="Test" → stem="test")
    std::string checkpoint_path = temp_dir + "/.needle/test/checkpoint.json";
    CHECK(needle::is_file(checkpoint_path));

    // Verify checkpoint content is valid JSON
    if (needle::is_file(checkpoint_path)) {
        std::ifstream f(checkpoint_path);
        std::ostringstream ss;
        ss << f.rdbuf();
        auto cp_json = nlohmann::json::parse(ss.str(), nullptr, false);
        CHECK_FALSE(cp_json.is_discarded());
        if (!cp_json.is_discarded()) {
            CHECK(cp_json.contains("completed_nodes"));
        }
    }

    remove_dir(temp_dir);
}

// ─── Error persistence in run view ───────────────────────────────────

TEST_CASE("ServerIntegration: failed run populates node_errors", "[integration][server]") {
    IntegrationTestServer ts(18855, make_fail_tool_registry());
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("error_persist");

    nlohmann::json body;
    body["dot_source"] = FAILING_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();

    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());
    CHECK(run_view["status"] == "failed");

    // node_errors should be populated
    CHECK(run_view.contains("node_errors"));
    if (run_view.contains("node_errors") && run_view["node_errors"].is_object()) {
        CHECK_FALSE(run_view["node_errors"].empty());
        // The "step" node should have the error
        if (run_view["node_errors"].contains("step")) {
            std::string err = run_view["node_errors"]["step"].get<std::string>();
            CHECK(err.find("intentional test failure") != std::string::npos);
        }
    }

    remove_dir(temp_dir);
}

// ─── Warnings in run view ────────────────────────────────────────────

TEST_CASE("ServerIntegration: unresolved variables produce warnings", "[integration][server]") {
    IntegrationTestServer ts(18856);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("warnings");

    // A DOT graph with an unresolved variable reference
    std::string dot_with_var =
        "digraph test_warn {\n"
        "    graph [goal=\"test warnings\", label=\"Test Warn\"]\n"
        "    start [shape=Mdiamond, label=\"Start\"]\n"
        "    step [label=\"Step\", handler=\"tool\", prompt=\"$var.missing_value\", command=\"echo ok\"]\n"
        "    exit [shape=Msquare, label=\"Done\"]\n"
        "    start -> step -> exit\n"
        "}\n";

    nlohmann::json body;
    body["dot_source"] = dot_with_var;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();

    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());

    // warnings array should be populated with the unresolved variable warning
    CHECK(run_view.contains("warnings"));
    if (run_view.contains("warnings") && run_view["warnings"].is_array()) {
        bool found_var_warning = false;
        for (const auto& w : run_view["warnings"]) {
            if (w.get<std::string>().find("var.missing_value") != std::string::npos) {
                found_var_warning = true;
            }
        }
        CHECK(found_var_warning);
    }

    remove_dir(temp_dir);
}

// ─── Dot-bearing run-id route matching ───────────────────────────────

// Regression for: project dirs with dots (e.g. "my.subproject")
// produce run IDs like "my.subproject-260427-1552". Several per-run
// endpoints used `[\w-]+` for the id, which excludes `.`, so the route
// never matched and httplib returned its default 404 with an empty body.
// The dashboard's `r.json()` then threw on the empty body, surfacing
// "Failed to submit answer" with no detail.
TEST_CASE("ServerIntegration: per-run endpoints accept dot-bearing run ids",
          "[integration][server][regression]") {
    IntegrationTestServer ts(18870);
    ts.start(fixtures::make_simple_graph());

    // We don't need a real run — we just need to prove the regex matches
    // and our handler runs (which returns its own JSON 404 body for
    // unknown ids). Pre-fix, the route wouldn't match at all and the
    // body would be empty.
    const std::string dotted_id = "my.subproject-260427-1552";

    auto check = [&](const std::string& path, const httplib::Result& res) {
        REQUIRE(res);
        // Either 200/201 (handler ran and accepted) or 404 with our JSON
        // body (handler ran and rejected). What we MUST NOT see is an
        // empty body — that would mean the route never matched.
        CHECK_FALSE(res->body.empty());
        // And the body must be JSON (handler-provided), not HTML/text.
        auto j = nlohmann::json::parse(res->body, nullptr, false);
        CHECK_FALSE(j.is_discarded());
        (void)path;
    };

    auto r1 = ts.client.Post(
        ("/api/v1/runs/" + dotted_id + "/answer").c_str(),
        "{\"selected_index\":0,\"raw_input\":\"\"}", "application/json");
    check("answer", r1);

    auto r2 = ts.client.Post(
        ("/api/v1/runs/" + dotted_id + "/continue").c_str(),
        "{\"result\":\"\"}", "application/json");
    check("continue", r2);

    auto r3 = ts.client.Get(
        ("/api/v1/runs/" + dotted_id + "/interactive").c_str());
    check("interactive (GET)", r3);

    auto r4 = ts.client.Post(
        ("/api/v1/runs/" + dotted_id + "/interactive/chat").c_str(),
        "{\"message\":\"hi\"}", "application/json");
    check("interactive/chat", r4);
}

// ─── DOT-on-disk policy ──────────────────────────────────────────────

TEST_CASE("ServerIntegration: dot_source run does NOT copy DOT to project root",
          "[integration][server]") {
    IntegrationTestServer ts(18860);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("no_root_copy");

    nlohmann::json body;
    body["dot_source"] = SIMPLE_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();
    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());

    // The label-derived filename must NOT appear in the project root.
    // SIMPLE_DOT_SOURCE has label="Test" → stem "test".
    CHECK_FALSE(needle::is_file(temp_dir + "/test.dot"));

    // The canonical stash should live under .needle/<stem>/source.dot.
    std::string stash = temp_dir + "/.needle/test/source.dot";
    CHECK(needle::is_file(stash));
    if (needle::is_file(stash)) {
        std::ifstream f(stash); std::ostringstream ss; ss << f.rdbuf();
        CHECK(ss.str() == SIMPLE_DOT_SOURCE);
    }

    // The response advertises that canonical path.
    auto post_body = nlohmann::json::parse(res->body);
    CHECK(post_body.contains("dot_path"));
    if (post_body.contains("dot_path")) {
        CHECK(post_body["dot_path"].get<std::string>() == stash);
    }

    remove_dir(temp_dir);
}

TEST_CASE("ServerIntegration: dot_path run reads from disk and writes nothing extra",
          "[integration][server]") {
    IntegrationTestServer ts(18861);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("dot_path_only");

    // Write the user's DOT to an arbitrary filename — the kind of
    // filename a label-derived heuristic would never invent.
    std::string user_dot = temp_dir + "/0424-mybranch.dot";
    {
        std::ofstream out(user_dot);
        out << SIMPLE_DOT_SOURCE;
    }

    nlohmann::json body;
    body["dot_path"] = user_dot;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();
    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());

    // No label-derived duplicate appeared.
    CHECK_FALSE(needle::is_file(temp_dir + "/test.dot"));
    // The user's file is still there with the original content.
    CHECK(needle::is_file(user_dot));
    {
        std::ifstream f(user_dot); std::ostringstream ss; ss << f.rdbuf();
        CHECK(ss.str() == SIMPLE_DOT_SOURCE);
    }

    // logs_root tracks the user's filename, not the graph label.
    CHECK(needle::is_directory(temp_dir + "/.needle/0424-mybranch"));
    CHECK_FALSE(needle::is_directory(temp_dir + "/.needle/test"));

    auto post_body = nlohmann::json::parse(res->body);
    CHECK(post_body["dot_path"].get<std::string>() == user_dot);

    remove_dir(temp_dir);
}

TEST_CASE("ServerIntegration: dot_path run errors on missing file",
          "[integration][server]") {
    IntegrationTestServer ts(18862);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("dot_path_missing");

    nlohmann::json body;
    body["dot_path"] = temp_dir + "/does-not-exist.dot";
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    CHECK(res->status == 400);
    auto j = nlohmann::json::parse(res->body);
    CHECK(j.contains("error"));

    remove_dir(temp_dir);
}

TEST_CASE("ServerIntegration: write-file saves content and creates parent dirs",
          "[integration][server]") {
    IntegrationTestServer ts(18863);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("write_file");
    std::string nested = temp_dir + "/nested/dir/out.dot";

    nlohmann::json body;
    body["path"] = nested;
    body["content"] = SIMPLE_DOT_SOURCE;

    auto res = ts.client.Post("/api/v1/write-file", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 200);

    CHECK(needle::is_file(nested));
    std::ifstream f(nested); std::ostringstream ss; ss << f.rdbuf();
    CHECK(ss.str() == SIMPLE_DOT_SOURCE);

    remove_dir(temp_dir);
}

// ─── Run view has expected structure ─────────────────────────────────

TEST_CASE("ServerIntegration: completed run view has full structure", "[integration][server]") {
    IntegrationTestServer ts(18857);
    ts.start(fixtures::make_simple_graph());

    std::string temp_dir = make_temp_dir("full_view");

    nlohmann::json body;
    body["dot_source"] = SIMPLE_DOT_SOURCE;
    body["project_dir"] = temp_dir;

    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    if (!res) { WARN("Could not connect to test server"); remove_dir(temp_dir); return; }
    REQUIRE(res->status == 201);
    auto run_id = nlohmann::json::parse(res->body)["id"].get<std::string>();

    auto run_view = wait_for_run(ts.client, run_id);
    REQUIRE_FALSE(run_view.empty());

    // Verify expected fields in the run view
    CHECK(run_view.contains("id"));
    CHECK(run_view.contains("status"));
    CHECK(run_view.contains("node_statuses"));
    CHECK(run_view.contains("node_errors"));
    CHECK(run_view.contains("warnings"));
    CHECK(run_view.contains("completed_stages"));
    CHECK(run_view.contains("total_stages"));
    CHECK(run_view.contains("event_count"));
    CHECK(run_view.contains("dot_source"));

    remove_dir(temp_dir);
}

#else

TEST_CASE("ServerIntegration: disabled when NEEDLE_ENABLE_SERVER not defined", "[integration][server]") {
    SUCCEED("Server integration tests skipped - NEEDLE_ENABLE_SERVER not defined");
}

#endif // NEEDLE_ENABLE_SERVER
