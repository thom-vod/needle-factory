#include <catch2/catch.hpp>

#ifdef NEEDLE_ENABLE_SERVER

#include "needle/backend/process_runner.h"
#include "needle/engine/run_guard.h"
#include "needle/config/needle_config.h"
#include "needle/server/http_server.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "needle/platform/platform.h"
#include "helpers/graph_fixtures.h"

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace needle;

namespace {

class FixedHandler : public Handler {
public:
    FixedHandler(std::string type, StageStatus status, int delay_ms = 0)
        : type_(std::move(type)), status_(status), delay_ms_(delay_ms) {}

    std::string type_name() const override { return type_; }

    Result<Outcome> execute(const Node&, Context&, const ExecutionContext&) override {
        if (delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        Outcome o;
        o.status = status_;
        o.output = status_ == StageStatus::SUCCESS ? "ok" : "failed";
        return Result<Outcome>::success(std::move(o));
    }

private:
    std::string type_;
    StageStatus status_;
    int delay_ms_;
};

std::shared_ptr<HandlerRegistry> make_registry(StageStatus codergen_status,
                                               int codergen_delay_ms = 0) {
    auto reg = std::make_shared<HandlerRegistry>();
    for (const auto& t : {"start", "exit", "parallel", "fan_in",
                          "conditional", "wait_human", "tool",
                          "manager_loop", "llmkit"}) {
        reg->register_handler(t, std::make_shared<FixedHandler>(t, StageStatus::SUCCESS));
    }
    reg->register_handler("codergen",
        std::make_shared<FixedHandler>("codergen", codergen_status, codergen_delay_ms));
    return reg;
}

Graph make_troubleshoot_graph() {
    Graph base = fixtures::make_simple_graph();
    AttributeMap attrs;
    attrs.set("troubleshoot_on_failure", "diagnose");
    return Graph::make(base.name(), base.nodes(), base.edges(), attrs);
}

class BlockingProcessRunner : public ProcessRunner {
public:
    explicit BlockingProcessRunner(int delay_ms) : delay_ms_(delay_ms) {}

    Result<ProcessResult> run(const std::string&, const std::vector<std::string>&,
                              const std::string&, int,
                              const std::map<std::string, std::string>& = {},
                              const std::string& = "", int = 0,
                              std::function<void(const std::string&)> stdout_callback = nullptr) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(delay_ms_), [&] { return killed_; });
        }
        ProcessResult pr;
        pr.exit_code = 0;
        pr.stdout_output =
            R"({"type":"result","subtype":"success","is_error":false,"result":"done","total_cost_usd":0.01})";
        if (stdout_callback) stdout_callback(pr.stdout_output + "\n");
        return Result<ProcessResult>::success(std::move(pr));
    }

    void kill_all() override {
        std::lock_guard<std::mutex> lock(mutex_);
        killed_ = true;
        cv_.notify_all();
    }

private:
    int delay_ms_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool killed_ = false;
};

struct TestServer {
    NeedleHttpServer server;
    httplib::Client client;
    std::string project_dir;

    TestServer(int port, StageStatus codergen_status, int codergen_delay_ms = 0,
               bool force_worker_throw = false, bool enable_auto_troubleshoot = false)
        : server(port, "127.0.0.1")
        , client("127.0.0.1", port)
        , project_dir(platform::temp_dir() + "/needle_ts_endpoint_" + std::to_string(port)) {
        platform::remove_recursive(project_dir);
        platform::mkdir_p(project_dir);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);

        PipelineConfig config;
        config.handler_registry = make_registry(codergen_status, codergen_delay_ms);
        config.edge_selector = std::make_shared<EdgeSelector>();
        config.process_runner = std::make_shared<BlockingProcessRunner>(600);
        server.disable_run_persistence();
        if (force_worker_throw) {
            server.set_troubleshoot_worker_test_hook(
                [](const std::string&, const std::string&) {
                    throw std::runtime_error("forced troubleshoot worker failure");
                });
        }
        EventBus bus;
        Graph graph = enable_auto_troubleshoot
            ? make_troubleshoot_graph()
            : fixtures::make_simple_graph();
        server.start(graph, std::move(config), bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ~TestServer() {
        server.stop();
        platform::remove_recursive(project_dir);
    }
};

std::string create_run(TestServer& ts) {
    nlohmann::json body;
    body["project_dir"] = ts.project_dir;
    auto res = ts.client.Post("/api/v1/runs", body.dump(), "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 201);
    return nlohmann::json::parse(res->body)["id"].get<std::string>();
}

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

std::string wait_for_auto_troubleshoot_session(const std::string& project_dir) {
    for (int i = 0; i < 50; ++i) {
        const std::string needle_dir = project_dir + "/.needle";
        if (platform::is_directory(needle_dir)) {
            auto stems = platform::list_directory(needle_dir);
            for (const auto& stem : stems) {
                const std::string troubleshoot_dir =
                    needle_dir + "/" + stem + "/troubleshoot";
                if (!platform::is_directory(troubleshoot_dir)) continue;
                auto entries = platform::list_directory(troubleshoot_dir);
                for (const auto& entry : entries) {
                    if (entry.rfind("session-", 0) == 0) {
                        return entry.substr(std::string("session-").size());
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return "";
}

std::string write_blocking_agent_script(const std::string& dir) {
    const std::string path = dir + "/blocking-agent.sh";
    std::ofstream out(path);
    out << "#!/bin/sh\n"
        << "printf '%s\\n' '{\"type\":\"assistant\",\"message\":\"started\"}'\n"
        << "sleep 30\n";
    out.close();
#ifndef _WIN32
    chmod(path.c_str(), 0755);
#endif
    return path;
}

struct ConfigRestore {
    std::string agent;
    std::string model;

    ConfigRestore()
        : agent(NeedleConfig::global().get_string("defaults.troubleshoot_agent"))
        , model(NeedleConfig::global().get_string("defaults.troubleshoot_model")) {}

    ~ConfigRestore() {
        NeedleConfig::global().set("defaults.troubleshoot_agent", agent);
        NeedleConfig::global().set("defaults.troubleshoot_model", model);
    }
};

} // anonymous namespace

TEST_CASE("Troubleshoot endpoint accepts failed run and rejects concurrent invoke",
          "[server][troubleshoot]") {
    TestServer ts(18820, StageStatus::FAILURE);
    std::string run_id = create_run(ts);
    REQUIRE(wait_for_status(ts, run_id, "failed") == "failed");

    nlohmann::json body;
    body["mode"] = "diagnose";
    body["trust"] = "snapshot";
    auto first = ts.client.Post("/api/v1/runs/" + run_id + "/troubleshoot",
                                body.dump(), "application/json");
    REQUIRE(first);
    REQUIRE(first->status == 202);
    auto first_json = nlohmann::json::parse(first->body);
    REQUIRE(first_json.contains("session_id"));

    auto second = ts.client.Post("/api/v1/runs/" + run_id + "/troubleshoot",
                                 body.dump(), "application/json");
    REQUIRE(second);
    REQUIRE(second->status == 409);
    auto err = nlohmann::json::parse(second->body);
    REQUIRE(err["error"] == "troubleshoot already in flight");

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
}

TEST_CASE("Troubleshoot endpoint rejects non-failed run", "[server][troubleshoot]") {
    TestServer ts(18821, StageStatus::SUCCESS, 800);
    std::string run_id = create_run(ts);

    nlohmann::json body;
    body["mode"] = "diagnose";
    auto res = ts.client.Post("/api/v1/runs/" + run_id + "/troubleshoot",
                              body.dump(), "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 400);
    auto err = nlohmann::json::parse(res->body);
    REQUIRE(err["error"] == "run is not in failed state");
}

TEST_CASE("Troubleshoot endpoint rejects run guard collision", "[server][troubleshoot]") {
    TestServer ts(18822, StageStatus::FAILURE);
    std::string run_id = create_run(ts);
    REQUIRE(wait_for_status(ts, run_id, "failed") == "failed");

    auto guard = RunGuard::try_reserve(run_id);
    REQUIRE(guard.has_value());

    nlohmann::json body;
    body["mode"] = "diagnose";
    auto res = ts.client.Post("/api/v1/runs/" + run_id + "/troubleshoot",
                              body.dump(), "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 409);
    auto err = nlohmann::json::parse(res->body);
    REQUIRE(err["error"] == "concurrent_session");
    REQUIRE(err["run_id"] == run_id);
}

TEST_CASE("Troubleshoot endpoint clears in-flight when worker throws",
          "[server][troubleshoot]") {
    TestServer ts(18823, StageStatus::FAILURE, 0, true);
    std::string run_id = create_run(ts);
    REQUIRE(wait_for_status(ts, run_id, "failed") == "failed");

    nlohmann::json body;
    body["mode"] = "diagnose";
    auto first = ts.client.Post("/api/v1/runs/" + run_id + "/troubleshoot",
                                body.dump(), "application/json");
    REQUIRE(first);
    REQUIRE(first->status == 202);

    bool accepted_again = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto retry = ts.client.Post("/api/v1/runs/" + run_id + "/troubleshoot",
                                    body.dump(), "application/json");
        REQUIRE(retry);
        if (retry->status == 202) {
            accepted_again = true;
            break;
        }
        REQUIRE(retry->status == 409);
    }
    REQUIRE(accepted_again);
}

TEST_CASE("Troubleshoot cancel reaches engine-auto registered runner",
          "[server][troubleshoot]") {
    ConfigRestore restore;
    TestServer ts(18824, StageStatus::FAILURE, 0, false, true);
    const std::string script = write_blocking_agent_script(ts.project_dir);
    NeedleConfig::global().set("defaults.troubleshoot_agent", script);
    NeedleConfig::global().set("defaults.troubleshoot_model", "test-model");

    std::string run_id = create_run(ts);
    std::string session_id = wait_for_auto_troubleshoot_session(ts.project_dir);
    REQUIRE_FALSE(session_id.empty());

    nlohmann::json body;
    body["session_id"] = session_id;
    auto cancel = ts.client.Post("/api/v1/runs/" + run_id + "/troubleshoot/cancel",
                                 body.dump(), "application/json");
    REQUIRE(cancel);
    REQUIRE(cancel->status == 200);
    auto j = nlohmann::json::parse(cancel->body);
    REQUIRE(j["session_id"] == session_id);
    REQUIRE(j["killed"] == true);

    REQUIRE(wait_for_status(ts, run_id, "failed") == "failed");
}

#else

TEST_CASE("Troubleshoot endpoint tests skipped without server", "[server][troubleshoot]") {
    SUCCEED("NEEDLE_ENABLE_SERVER not defined");
}

#endif
