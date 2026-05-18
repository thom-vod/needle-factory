#ifdef NEEDLE_ENABLE_SERVER

#include "needle/server/http_server.h"
#include "needle/platform/portable_time.h"
#include "needle/server/model_cache.h"
#include "needle/server/dashboard_html.h"
#include "needle/config/needle_config.h"
#include "needle/util/context_defaults.h"
#include "needle/util/graph_serializer.h"
#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/parser/stylesheet_parser.h"
#include "needle/validation/graph_validator.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/engine/auto_troubleshoot.h"
#include "needle/engine/run_guard.h"
#include "needle/engine/transform.h"
#include "interviewer/http_interviewer.h"
#include "needle/handlers/handler_registry.h"
#include "needle/handlers/all_handlers.h"
#include "needle/backend/process_runner.h"
#include "needle/engine/troubleshoot_backup.h"

#include "needle/util/resource_locator.h"
#include "needle/util/fs_helpers.h"
#include "needle/util/logger.h"
#include "needle/util/curl_client.h"
#include "needle/util/timestamp.h"
#include "needle/troubleshoot/stream_parser.h"
#include "needle/troubleshoot/session_id.h"
#include "needle/troubleshoot/types.h"
#include "needle/worktree/strategy.h"

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include "needle/platform/platform.h"

#ifdef NEEDLE_HAS_CURL
#include <curl/curl.h>
#endif

namespace needle {

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string trim_copy(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string yaml_scalar_to_string(std::string value) {
    value = trim_copy(value);
    if (value == "null" || value == "~") return "";
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < value.size(); ++i) {
            if (value[i] == '\\' && i + 2 < value.size()) {
                ++i;
            }
            out.push_back(value[i]);
        }
        return out;
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string absolute_path(std::string path) {
    if (path.empty() || platform::is_absolute_path(path)) return path;
    return platform::path_join(platform::getcwd_str(), path);
}

TroubleshootMode configured_troubleshoot_mode() {
    std::string configured = NeedleConfig::global().get_string("defaults.troubleshoot_mode");
    if (configured.empty()) return TroubleshootMode::Off;
    Maybe<TroubleshootMode> parsed = parse_troubleshoot_mode(configured);
    return parsed.has_value() ? *parsed : TroubleshootMode::Off;
}

TroubleshootMode resolve_troubleshoot_mode(const Graph& graph) {
    TroubleshootMode mode = configured_troubleshoot_mode();
    std::string attr = graph.graph_attrs().get("troubleshoot_on_failure");
    if (!attr.empty()) {
        Maybe<TroubleshootMode> parsed = parse_troubleshoot_mode_graph_attr(attr);
        if (parsed.has_value()) mode = *parsed;
    }
    return mode;
}

Result<Graph> load_troubleshoot_graph(const std::string& graph_path) {
    if (graph_path.empty()) {
        return Result<Graph>::success(Graph::make("manual_troubleshoot", {}, {}));
    }
    std::ifstream in(graph_path);
    if (!in.is_open()) {
        return Result<Graph>::success(Graph::make("manual_troubleshoot", {}, {}));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    DotParser parser(ss.str());
    auto parsed = parser.parse();
    if (!parsed.ok()) return Result<Graph>::failure(parsed.error());
    GraphBuilder builder;
    return builder.build(parsed.value());
}

void write_troubleshoot_cancel_marker(const std::string& session_dir) {
    if (session_dir.empty()) return;
    platform::mkdir_p(session_dir);
    nlohmann::json marker;
    marker["status"] = to_string(TroubleshootSessionStatus::Cancelled);
    marker["written_at"] = utc_timestamp_now();
    marker["reason"] = "cancelled";
    std::ofstream out(session_dir + "/cancel.json");
    if (out.is_open()) out << marker.dump(2);
}

std::string reserve_troubleshoot_session_id(const std::string& run_dir) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string session_id = make_troubleshoot_session_id();
        std::string session_dir = run_dir + "/troubleshoot/session-" + session_id;
        if (!platform::file_exists(session_dir) && !platform::is_directory(session_dir)) {
            return session_id;
        }
    }
    return make_troubleshoot_session_id();
}

nlohmann::json activity_from_events_ndjson(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return nlohmann::json::array();

    TroubleshootStreamParser parser;
    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto events = parser.parse_line(line);
        for (const auto& e : events) {
            if (e.type != "tool_call" &&
                e.type != "tool_result" &&
                e.type != "session_started" &&
                e.type != "session_completed" &&
                e.type != "session_escalated" &&
                e.type != "cost_update" &&
                e.type != "report_written") {
                continue;
            }
            nlohmann::json row = e.payload;
            row["type"] = e.type;
            rows.push_back(std::move(row));
        }
    }

    const size_t kCap = 50;
    if (rows.size() > kCap) rows.erase(rows.begin(), rows.end() - kCap);

    nlohmann::json out = nlohmann::json::array();
    for (auto& row : rows) out.push_back(std::move(row));
    return out;
}

nlohmann::json troubleshoot_view_from_logs_root(const std::string& logs_root) {
    const std::string troubleshoot_dir = logs_root + "/troubleshoot";
    if (logs_root.empty() || !platform::is_directory(troubleshoot_dir)) return nlohmann::json();

    auto entries = platform::list_directory(troubleshoot_dir);
    std::sort(entries.begin(), entries.end(), std::greater<std::string>());

    for (const auto& entry : entries) {
        if (entry.find("session-") != 0) continue;
        const std::string session_dir = troubleshoot_dir + "/" + entry;
        if (!platform::is_directory(session_dir)) continue;
        const std::string recovery_path = session_dir + "/recovery.md";
        if (!platform::file_exists(recovery_path)) continue;

        std::ifstream in(recovery_path);
        if (!in.is_open()) continue;

        std::string line;
        if (!std::getline(in, line) || trim_copy(line) != "---") continue;

        nlohmann::json view = nlohmann::json::object();
        while (std::getline(in, line)) {
            if (trim_copy(line) == "---") break;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = trim_copy(line.substr(0, colon));
            const std::string value = yaml_scalar_to_string(line.substr(colon + 1));

            if (key == "cost_usd") {
                char* end = nullptr;
                const double cost = std::strtod(value.c_str(), &end);
                if (end != value.c_str()) view[key] = cost;
            } else if (key == "session_id" || key == "tier" || key == "outcome" ||
                       key == "failed_node" || key == "backup_branch" ||
                       key == "backup_base" || key == "escalate_reason") {
                if (!value.empty()) view[key] = value;
            }
        }

        if (!view.empty()) {
            const std::string relative_session_dir = "troubleshoot/" + entry;
            view["session_dir"] = relative_session_dir;
            view["report_path"] = relative_session_dir + "/recovery.md";
            view["activity"] = activity_from_events_ndjson(session_dir + "/events.ndjson");
            return view;
        }
    }

    return nlohmann::json();
}

std::string troubleshoot_sse_name(const PipelineEvent& event) {
    if (event.type != EventType::TROUBLESHOOT_ACTIVITY) return "";
    if (!event.data.contains("event_type") || !event.data["event_type"].is_string()) {
        return "troubleshoot.raw";
    }
    return "troubleshoot." + event.data["event_type"].get<std::string>();
}

nlohmann::json troubleshoot_sse_payload(const std::string& run_id,
                                        const PipelineEvent& event) {
    nlohmann::json payload = nlohmann::json::object();
    if (event.data.contains("payload") && event.data["payload"].is_object()) {
        payload = event.data["payload"];
    }
    payload["type"] = troubleshoot_sse_name(event);
    payload["timestamp"] = event.timestamp;
    payload["node_id"] = event.node_id;
    payload["run_id"] = run_id;
    if (event.data.contains("session_id")) payload["session_id"] = event.data["session_id"];
    if (event.data.contains("event_type")) payload["event_type"] = event.data["event_type"];
    return payload;
}

std::string format_sse_frame(const std::string& event_name,
                             const nlohmann::json& payload) {
    std::string out;
    if (!event_name.empty()) {
        out += "event: " + event_name + "\n";
    }
    out += "data: " + payload.dump() + "\n\n";
    return out;
}

std::string format_pipeline_sse(const std::string& run_id,
                                const PipelineEvent& event,
                                size_t seq = static_cast<size_t>(-1)) {
    const std::string event_name = troubleshoot_sse_name(event);
    nlohmann::json payload;
    if (!event_name.empty()) {
        payload = troubleshoot_sse_payload(run_id, event);
    } else {
        payload = event.to_json();
        if (!run_id.empty()) payload["run_id"] = run_id;
    }
    if (seq != static_cast<size_t>(-1)) payload["seq"] = seq;
    return format_sse_frame(event_name, payload);
}

std::shared_ptr<Transform> parse_inline_stylesheet(const Graph& graph) {
    std::string ss_source = graph.graph_attrs().get("model_stylesheet");
    if (ss_source.empty()) return nullptr;
    NEEDLE_LOG_DEBUG("stylesheet", "model_stylesheet (%zu bytes): [%s]",
                     ss_source.size(), ss_source.c_str());
    auto ss_result = StylesheetParser::parse(ss_source);
    if (!ss_result.ok()) {
        NEEDLE_LOG_WARN("stylesheet", "model_stylesheet parse failed: %s",
                        ss_result.error().c_str());
        return nullptr;
    }
    return make_stylesheet_transform(std::move(ss_result.value()));
}

Result<void> apply_configured_stylesheets(Graph& graph, const std::string& stylesheet_path) {
    Context ctx;

    if (auto t = parse_inline_stylesheet(graph)) {
        auto result = t->apply(graph, ctx);
        if (!result.ok()) {
            return result;
        }
    }

    if (!stylesheet_path.empty()) {
        std::string ss_source = read_file(stylesheet_path);
        if (ss_source.empty()) {
            return Result<void>::failure("cannot read stylesheet: " + stylesheet_path);
        }
        auto ss_result = StylesheetParser::parse(ss_source);
        if (!ss_result.ok()) {
            return Result<void>::failure("stylesheet: " + ss_result.error());
        }
        auto transform = make_stylesheet_transform(std::move(ss_result.value()));
        auto result = transform->apply(graph, ctx);
        if (!result.ok()) {
            return result;
        }
    }

    return Result<void>::success();
}

// M6: Use the extracted ModelCache class (thread-safe, no TOCTOU)
static needle::ModelCache& model_cache_instance() {
    static needle::ModelCache cache;
    return cache;
}

// Hardcoded fallback model lists per provider.
nlohmann::json fallback_models(const std::string& provider) {
    if (provider == "openai") {
        return nlohmann::json::array({
            "gpt-4o", "gpt-4o-mini", "o1", "o1-mini", "o3-mini"
        });
    } else if (provider == "anthropic") {
        return nlohmann::json::array({
            "claude-opus-4-7", "claude-sonnet-4-6", "claude-haiku-4-5"
        });
    } else if (provider == "gemini") {
        return nlohmann::json::array({
            "gemini-2.5-pro", "gemini-2.5-flash", "gemini-2.0-flash"
        });
    }
    return nlohmann::json::array();
}

#ifdef NEEDLE_HAS_CURL
static size_t model_curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    std::string* buf = static_cast<std::string*>(ud);
    size_t total = size * nmemb;
    buf->append(ptr, total);
    return total;
}

// Fetch models from a provider's API. Returns a JSON array of model name strings.
nlohmann::json fetch_models_from_api(const std::string& provider, const std::string& api_key) {
    if (provider == "anthropic") {
        // No model list API -- always use hardcoded
        return fallback_models(provider);
    }

    CURL* curl = curl_easy_init();
    if (!curl) return fallback_models(provider);

    std::string response;
    std::string url;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (provider == "openai") {
        url = "https://api.openai.com/v1/models";
        std::string auth = "Authorization: Bearer " + api_key;
        headers = curl_slist_append(headers, auth.c_str());
    } else if (provider == "gemini") {
        url = "https://generativelanguage.googleapis.com/v1beta/models";
        std::string gemini_auth = "x-goog-api-key: " + api_key;
        headers = curl_slist_append(headers, gemini_auth.c_str());
    } else {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return fallback_models(provider);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, model_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        return fallback_models(provider);
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        nlohmann::json model_names = nlohmann::json::array();

        if (provider == "openai" && j.contains("data") && j["data"].is_array()) {
            for (const auto& m : j["data"]) {
                if (m.contains("id") && m["id"].is_string()) {
                    std::string id = m["id"].get<std::string>();
                    // Filter to relevant models (gpt-4*, o1*, o3*)
                    if (id.find("gpt-4") == 0 || id.find("o1") == 0 ||
                        id.find("o3") == 0 || id.find("gpt-3.5") == 0) {
                        model_names.push_back(id);
                    }
                }
            }
            if (model_names.empty()) return fallback_models(provider);
            return model_names;
        }

        if (provider == "gemini" && j.contains("models") && j["models"].is_array()) {
            for (const auto& m : j["models"]) {
                if (m.contains("name") && m["name"].is_string()) {
                    std::string name = m["name"].get<std::string>();
                    // Strip "models/" prefix
                    if (name.find("models/") == 0) {
                        name = name.substr(7);
                    }
                    model_names.push_back(name);
                }
            }
            if (model_names.empty()) return fallback_models(provider);
            return model_names;
        }
    } catch (...) {
        // Fall through
    }

    return fallback_models(provider);
}
#endif // NEEDLE_HAS_CURL

} // anonymous namespace

NeedleHttpServer::NeedleHttpServer(int port, const std::string& bind_addr)
    : port_(port)
    , bind_addr_(bind_addr)
    , running_(false)
    , graph_(Graph::make("", {}, {}))
    , global_queue_(std::make_shared<GlobalEventQueue>()) {}

NeedleHttpServer::~NeedleHttpServer() {
    stop();
}

std::string NeedleHttpServer::generate_run_id(const std::string& project_dir) {
    return RunRegistry::generate_run_id(project_dir);
}

std::shared_ptr<PipelineRun> NeedleHttpServer::create_run(
        const Graph& run_graph, const std::string& dot_source,
        const std::string& project_dir, const std::map<std::string, std::string>& vars,
        const std::string& stem_override, const std::string& graph_file) {
    std::string run_id = generate_run_id(project_dir);
    auto run = std::make_shared<PipelineRun>();
    run->id = run_id;
    run->dot_source = dot_source;
    run->set_status("running");

    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        runs_[run_id] = run;
    }
    active_runs_.fetch_add(1);

    // Wire up collector to record events
    auto run_ptr = run;
    auto gq = global_queue_;
    run->event_bus.subscribe([run_ptr, gq](const PipelineEvent& e) {
        run_ptr->collector.record(e);

        // Append to global SSE queue
        {
            std::lock_guard<std::mutex> lock(gq->mutex);
            std::string data = format_pipeline_sse(run_ptr->id, e, gq->sequence++);
            gq->events.push_back(std::move(data));
        }
    });

    // Dispatch per-event to any registered observers.
    if (!observers_.empty()) {
        std::string run_id = run->id;
        run->event_bus.subscribe([this, run_id](const PipelineEvent& e) {
            notify_run_event(run_id, e);
        });
    }

    // Create per-run HttpInterviewer so human gates can be answered via API
    auto http_interviewer = std::make_shared<HttpInterviewer>();
    run->interviewer = http_interviewer;

    // Create per-run interactive session
    run->interactive_session = std::make_shared<InteractiveSession>();

    // Re-register handlers that need the per-run interviewer / session
    PipelineConfig config_copy = config_;
    config_copy.interactive_session = run->interactive_session;
    config_copy.pause_controller = pause_controller_;
    if (config_copy.handler_registry) {
        auto registry_copy = std::make_shared<HandlerRegistry>(*config_copy.handler_registry);
        registry_copy->register_handler("wait_human", make_wait_human_handler(http_interviewer));
        registry_copy->register_handler("interactive",
            make_interactive_handler(config_copy.cli_backend, run->interactive_session));
        config_copy.handler_registry = registry_copy;
    }

    // Derive dot_stem and set up per-DOT logs root. A caller-provided
    // override wins over the label-derived stem, so if the user has an
    // existing <name>.dot on disk whose content matches, we align
    // logs_root (.needle/<name>/) with that filename instead of with the
    // graph's label.
    std::string stem = stem_override.empty()
        ? dot_stem_from_source(dot_source)
        : stem_override;
    run->dot_stem = stem;
    run->project_dir = project_dir;
    run->created_at = utc_timestamp_now();

    if (!project_dir.empty() && project_dir != ".") {
        config_copy.project_dir = project_dir;

        // Migrate flat .needle/ to per-DOT subdirectory if needed
        migrate_flat_needle_dir(project_dir, stem);

        config_copy.logs_root = compute_logs_root(project_dir, stem);
        platform::mkdir_p(config_copy.logs_root);
        config_copy.checkpoint_writer = std::make_shared<JsonCheckpointWriter>();
        run->logs_root = config_copy.logs_root;

        // Don't write a copy of the DOT source — the resume endpoint
        // accepts dot_source in the request body and the dashboard sends
        // it. Writing a copy under the graph name (e.g. diskowl.dot from
        // project.dot) creates confusing duplicates that go stale.
    } else {
        run->logs_root = config_copy.logs_root;
    }

    // Record the canonical graph path on the config so the engine writes
    // it into the checkpoint. Resume can then locate the DOT via cp.graph_file
    // even when the dashboard doesn't re-supply dot_path/dot_source.
    if (!graph_file.empty()) {
        config_copy.graph_file = graph_file;
    }
        config_copy.troubleshoot_mode = resolve_troubleshoot_mode(run_graph);
        config_copy.auto_troubleshoot = config_copy.troubleshoot_mode != TroubleshootMode::Off;
    config_copy.troubleshoot_register_runner =
        [this](const std::string& run_id,
               const std::string& session_id,
               std::shared_ptr<ProcessRunner> runner) {
            std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
            auto it = troubleshoot_in_flight_.find(run_id);
            if (it != troubleshoot_in_flight_.end() && it->second.active &&
                it->second.session_id != session_id) {
                NEEDLE_LOG_WARN("troubleshoot",
                                "runner registration ignored for run=%s session=%s; active session=%s",
                                run_id.c_str(), session_id.c_str(),
                                it->second.session_id.c_str());
                return;
            }
            TroubleshootInFlight& inflight = troubleshoot_in_flight_[run_id];
            inflight.active = true;
            inflight.session_id = session_id;
            inflight.agent_pid = 0;
            inflight.runner = std::move(runner);
        };
    // Record the content hash so resume can detect on-disk edits.
    config_copy.dot_content_hash = std::to_string(std::hash<std::string>{}(dot_source));

    // Persist to run registry
    run_registry_->add_entry(run->id, run->dot_stem, run->dot_source,
                                 run->project_dir, run->logs_root,
                                 run->get_status(), run->created_at, run->dry_run);
    run_registry_->save();

    // Start pipeline in background thread
    Graph graph_copy = run_graph;
    auto run_vars = vars;
    auto run_project_dir = project_dir;
    auto run_logs_root = config_copy.logs_root;
    auto run_dot_stem = run->dot_stem;
    auto registry = run_registry_;
    run->run_thread = std::thread([this, run_ptr, graph_copy, config_copy, run_vars, run_project_dir, run_logs_root, run_dot_stem, registry]() mutable {
        // Use run_ptr->cancelled as the engine's cancellation flag so the
        // HTTP cancel endpoint directly controls the engine loop.
        PipelineEngine engine(std::move(config_copy), run_ptr->cancelled);
        Context ctx;
        ctx.set("needle.project_dir", run_project_dir);
        ctx.set("needle.run_id", run_ptr->id);
        if (!config_copy.graph_file.empty()) {
            ctx.set("needle.graph_path", config_copy.graph_file);
        }
        if (!run_logs_root.empty()) {
            ctx.set("needle.logs_root", run_logs_root);
            ctx.set("needle.logs_dir", run_logs_root + "/logs");
            platform::mkdir_p(run_logs_root + "/logs");
        }
        if (!run_dot_stem.empty()) {
            ctx.set("needle.dot_stem", run_dot_stem);
        }
        for (const auto& kv : run_vars) {
            ctx.set("var." + kv.first, kv.second);
        }
        inject_config_defaults(ctx, NeedleConfig::global(), true);
        auto result = engine.run(graph_copy, ctx, run_ptr->event_bus);
        std::string status;
        std::string error;
        if (run_ptr->cancelled.load()) {
            status = "cancelled";
        } else if (result.ok()) {
            status = "completed";
        } else {
            status = "failed";
            error = result.error();
        }
        run_ptr->set_status(status);
        if (!error.empty()) run_ptr->set_error(error);
        registry->update_status(run_ptr->id, status, error);
        registry->save();
        {
            std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
            auto it = troubleshoot_in_flight_.find(run_ptr->id);
            if (it != troubleshoot_in_flight_.end()) {
                it->second.active = false;
                it->second.runner.reset();
            }
        }
        if (active_runs_.fetch_sub(1) == 1) {
            notify_idle();
        }
    });
    // M9: Do NOT detach — thread is joined in stop() or PipelineRun destructor

    return run;
}

nlohmann::json NeedleHttpServer::derive_run_view(const PipelineRun& run) const {
    nlohmann::json rv;
    rv["id"] = run.id;
    rv["status"] = run.get_status();
    std::string error = run.get_error();
    if (!error.empty()) rv["error"] = error;

    auto events = run.collector.events();

    std::string current_node;
    size_t completed_stages = 0;
    std::map<std::string, std::string> node_statuses;
    std::string pending_question;
    std::string start_ts;
    std::string end_ts;
    nlohmann::json node_errors = nlohmann::json::object();
    nlohmann::json warnings = nlohmann::json::array();

    // Count actionable stages from the run's own DOT source (cached)
    size_t total_stages = run.cached_total_stages;
    if (total_stages == 0 && !run.dot_source.empty()) {
        DotParser parser(run.dot_source);
        auto ast = parser.parse();
        if (ast.ok()) {
            GraphBuilder builder;
            auto gr = builder.build(ast.value());
            if (gr.ok()) {
                for (const auto& n : gr.value().nodes()) {
                    if (n.type != NodeType::START && n.type != NodeType::EXIT)
                        ++total_stages;
                }
            }
        }
        run.cached_total_stages = total_stages;
    }
    // Fallback to server-wide graph
    if (total_stages == 0) {
        for (const auto& n : graph_.nodes()) {
            if (n.type != NodeType::START && n.type != NodeType::EXIT) {
                ++total_stages;
            }
        }
    }

    for (const auto& e : events) {
        switch (e.type) {
            case EventType::PIPELINE_STARTED:
                start_ts = e.timestamp;
                break;
            case EventType::PIPELINE_COMPLETED:
                end_ts = e.timestamp;
                break;
            case EventType::PIPELINE_FAILED:
                end_ts = e.timestamp;
                // Clear any nodes still marked "running" — the pipeline is done
                for (auto& kv : node_statuses) {
                    if (kv.second == "running") {
                        kv.second = "failed";
                    }
                }
                current_node.clear();
                break;
            case EventType::STAGE_STARTED:
                if (!e.node_id.empty()) {
                    node_statuses[e.node_id] = "running";
                    current_node = e.node_id;
                }
                break;
            case EventType::STAGE_COMPLETED:
                if (!e.node_id.empty()) {
                    node_statuses[e.node_id] = "completed";
                    ++completed_stages;
                    if (current_node == e.node_id) current_node.clear();
                }
                break;
            case EventType::STAGE_FAILED:
                if (!e.node_id.empty()) {
                    node_statuses[e.node_id] = "failed";
                    if (current_node == e.node_id) current_node.clear();
                    // Extract error detail for persistence
                    std::string error_detail = e.message;
                    if (e.data.contains("error")) {
                        error_detail = e.data["error"].get<std::string>();
                    }
                    node_errors[e.node_id] = error_detail;
                }
                break;
            case EventType::STAGE_WARNING:
            case EventType::VARIABLE_UNRESOLVED:
            case EventType::RESUME_WARNING:
                warnings.push_back(e.message);
                break;
            case EventType::STAGE_RETRYING:
                if (!e.node_id.empty()) {
                    node_statuses[e.node_id] = "running";
                }
                break;
            case EventType::HUMAN_QUESTION:
                // Don't surface interactive-session prompts as pending_question —
                // those are served separately via /interactive and rendered in
                // the chat panel. If we set pending_question here the UI would
                // show BOTH the chat panel and the wait-human gate banner.
                if (!(e.data.is_object() && e.data.value("interactive", false))) {
                    pending_question = e.message;
                }
                break;
            case EventType::HUMAN_ANSWER:
                pending_question.clear();
                break;
            default:
                break;
        }
    }

    rv["current_node"] = current_node;
    rv["completed_stages"] = completed_stages;
    rv["total_stages"] = total_stages;
    rv["pending_question"] = pending_question;
    rv["dot_source"] = run.dot_source;
    rv["project_dir"] = run.project_dir;
    rv["dot_stem"] = run.dot_stem;
    rv["dry_run"] = run.dry_run;

    nlohmann::json ns = nlohmann::json::object();
    for (const auto& kv : node_statuses) {
        ns[kv.first] = kv.second;
    }
    rv["node_statuses"] = ns;
    rv["node_errors"] = node_errors;
    rv["warnings"] = warnings;

    // Compute elapsed from event timestamps
    double elapsed = 0.0;
    if (!start_ts.empty()) {
        std::string ref_ts = end_ts.empty() ? utc_timestamp_now() : end_ts;
        // Parse ISO 8601 timestamps to compute difference
        std::tm tm_start = {}, tm_end = {};
        if (strptime(start_ts.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_start) &&
            strptime(ref_ts.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_end)) {
            elapsed = difftime(timegm(&tm_end), timegm(&tm_start));
            if (elapsed < 0) elapsed = 0;
        }
    }
    rv["elapsed_seconds"] = elapsed;
    rv["event_count"] = events.size();
    auto troubleshoot = troubleshoot_view_from_logs_root(run.logs_root);
    if (!troubleshoot.is_null()) rv["troubleshoot"] = troubleshoot;

    return rv;
}

void NeedleHttpServer::start(const Graph& graph, PipelineConfig config, EventBus& /*global_bus*/) {
    NEEDLE_LOG_INFO("server", "starting HTTP server on %s:%d", bind_addr_.c_str(), port_);
    graph_ = graph;
    config_ = std::move(config);
    active_runs_.store(0);
    running_.store(true);

    // Startup validation: log resource inventory
    auto templates_result = config_.resource_locator.find_dir("sample_dots");
    if (templates_result.ok()) {
        NEEDLE_LOG_INFO("server", "Templates directory: %s", templates_result.value().c_str());
    } else {
        NEEDLE_LOG_WARN("server", "Templates not found: %s", templates_result.error().c_str());
    }

    auto scripts_result = config_.resource_locator.find_dir("scripts");
    if (scripts_result.ok()) {
        NEEDLE_LOG_INFO("server", "Scripts directory: %s", scripts_result.value().c_str());
    } else {
        NEEDLE_LOG_WARN("server", "Scripts not found: %s", scripts_result.error().c_str());
    }

    // Load persisted runs from registry
    run_registry_->load();
    {
        auto persisted = run_registry_->all();
        bool registry_changed = false;
        for (const auto& pr : persisted) {
            std::string rid = pr.value("id", "");
            if (rid.empty()) continue;

            // Skip runs whose project directory no longer exists
            std::string pdir = pr.value("project_dir", "");
            if (!pdir.empty() && !platform::is_directory(pdir)) {
                NEEDLE_LOG_DEBUG("server", "skipping persisted run %s: project dir gone", rid.c_str());
                continue;
            }

            auto run = std::make_shared<PipelineRun>();
            run->id = rid;
            run->dot_source = pr.value("dot_source", "");
            run->dot_stem = pr.value("dot_stem", "");
            run->project_dir = pr.value("project_dir", "");
            run->logs_root = pr.value("logs_root", "");
            run->dry_run = pr.value("dry_run", false);
            run->created_at = pr.value("created_at", "");

            std::string status = pr.value("status", "");
            if (status == "running") {
                // Was interrupted by restart
                run->set_status("failed");
                run->set_error("Server restarted while run was in progress");
                run_registry_->update_status(rid, "failed", "Server restarted while run was in progress");
                registry_changed = true;
            } else {
                run->set_status(status);
                run->set_error(pr.value("error", ""));
            }

            // Reconstruct collector events from stage artifacts on disk
            // so the UI can show node statuses and graph coloring.
            if (!run->logs_root.empty()) {
                // Resolve relative logs_root against project_dir
                std::string abs_logs_root = run->logs_root;
                if (!platform::is_absolute_path(abs_logs_root) && !run->project_dir.empty()) {
                    abs_logs_root = run->project_dir + "/" + abs_logs_root;
                }
                std::string stages_dir = abs_logs_root + "/stages";
                std::string cp_path = abs_logs_root + "/checkpoint.json";

                // Read checkpoint for completed_nodes list and timestamp
                std::ifstream cp_in(cp_path);
                if (cp_in.is_open()) {
                    auto cp_json = nlohmann::json::parse(cp_in, nullptr, false);
                    if (cp_json.is_object()) {
                        // Emit synthetic PIPELINE_STARTED
                        PipelineEvent start_evt;
                        start_evt.type = EventType::PIPELINE_STARTED;
                        start_evt.timestamp = cp_json.value("timestamp", run->created_at);
                        start_evt.message = "Pipeline started (reconstructed)";
                        run->collector.record(start_evt);

                        // Emit STAGE_COMPLETED for each completed node
                        if (cp_json.contains("completed_nodes") && cp_json["completed_nodes"].is_array()) {
                            for (const auto& node_id_val : cp_json["completed_nodes"]) {
                                std::string nid = node_id_val.get<std::string>();
                                PipelineEvent stage_evt;
                                stage_evt.type = EventType::STAGE_COMPLETED;
                                stage_evt.node_id = nid;
                                stage_evt.timestamp = cp_json.value("timestamp", "");
                                stage_evt.message = "Stage completed: " + nid;
                                run->collector.record(stage_evt);
                            }
                        }

                        // Check for failed current_node (not in completed_nodes)
                        std::string current = cp_json.value("current_node", "");
                        if (!current.empty()) {
                            std::string stage_status_path = stages_dir + "/" + current + "/status.json";
                            std::ifstream sf(stage_status_path);
                            if (sf.is_open()) {
                                auto sj = nlohmann::json::parse(sf, nullptr, false);
                                if (sj.is_object() && sj.value("status", "") == "FAILURE") {
                                    PipelineEvent fail_evt;
                                    fail_evt.type = EventType::STAGE_FAILED;
                                    fail_evt.node_id = current;
                                    fail_evt.timestamp = cp_json.value("timestamp", "");
                                    fail_evt.message = sj.value("output", "Stage failed");
                                    run->collector.record(fail_evt);
                                }
                            }
                        }

                        // Emit PIPELINE_COMPLETED or PIPELINE_FAILED
                        if (status == "completed") {
                            PipelineEvent end_evt;
                            end_evt.type = EventType::PIPELINE_COMPLETED;
                            end_evt.timestamp = cp_json.value("timestamp", "");
                            end_evt.message = "Pipeline completed (reconstructed)";
                            run->collector.record(end_evt);
                        } else if (status == "failed") {
                            PipelineEvent end_evt;
                            end_evt.type = EventType::PIPELINE_FAILED;
                            end_evt.timestamp = cp_json.value("timestamp", "");
                            end_evt.message = run->get_error();
                            run->collector.record(end_evt);
                        }
                    }
                }
            }

            std::lock_guard<std::mutex> lock(runs_mutex_);
            runs_[rid] = run;
        }
        if (registry_changed) run_registry_->save();
        if (!persisted.empty()) {
            NEEDLE_LOG_INFO("server", "loaded %zu persisted runs from registry", persisted.size());
        }
    }

    // Pre-render graph SVG and dashboard page
    cached_dot_ = graph_to_dot(graph_);
    cached_svg_ = graph_to_svg(graph_);
    cached_page_ = dashboard::assemble_page(cached_svg_, graph_.name());

    auto svr_ptr = std::make_shared<httplib::Server>();
    svr_ptr_ = svr_ptr;
    stop_fn_ = [svr_ptr]() { svr_ptr->stop(); };

    server_thread_ = std::thread([this, svr_ptr]() {
        httplib::Server& svr = *svr_ptr;

        // ── Dashboard ──────────────────────────────────────────
        svr.Get("/", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_header("Content-Security-Policy",
                "default-src 'self'; "
                "script-src 'self' 'unsafe-inline' https://unpkg.com https://cdnjs.cloudflare.com; "
                "style-src 'self' 'unsafe-inline' https://cdnjs.cloudflare.com; "
                "connect-src 'self'; "
                "img-src 'self' data:");
            res.set_content(cached_page_, "text/html; charset=utf-8");
        });

        // ── /api/v1/status ─────────────────────────────────────
        svr.Get("/api/v1/status", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            nlohmann::json j;
            j["graph_name"] = graph_.name();
            j["node_count"] = graph_.nodes().size();
            j["edge_count"] = graph_.edges().size();
            j["dot_available"] = !cached_svg_.empty();
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                size_t active = 0;
                for (const auto& p : runs_) {
                    if (p.second->get_status() == "running") ++active;
                }
                j["active_runs"] = active;
            }
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/runs (GET) ─────────────────────────────────
        svr.Get("/api/v1/runs", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            nlohmann::json arr = nlohmann::json::array();
            std::lock_guard<std::mutex> lock(runs_mutex_);
            for (const auto& pair : runs_) {
                if (pair.second->collector.events().empty()) {
                    arr.push_back(reconstruct_run_view_from_disk(*pair.second));
                } else {
                    arr.push_back(derive_run_view(*pair.second));
                }
            }
            res.set_content(arr.dump(), "application/json");
        });

        // ── /api/v1/runs/:id (GET) ────────────────────────────
        svr.Get(R"(/api/v1/runs/([\w.-]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            std::lock_guard<std::mutex> lock(runs_mutex_);
            auto it = runs_.find(run_id);
            if (it == runs_.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"not found\"}", "application/json");
                return;
            }
            if (it->second->collector.events().empty()) {
                res.set_content(reconstruct_run_view_from_disk(*it->second).dump(), "application/json");
            } else {
                res.set_content(derive_run_view(*it->second).dump(), "application/json");
            }
        });

        svr.Post(R"(/api/v1/runs/([\w.-]+)/troubleshoot)",
                 [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() && !req.body.empty()) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid JSON\"}", "application/json");
                return;
            }
            if (body.is_discarded()) body = nlohmann::json::object();

            TroubleshootMode mode = TroubleshootMode::Tweak;
            if (body.contains("mode") && body["mode"].is_string()) {
                auto parsed = parse_troubleshoot_mode(body["mode"].get<std::string>());
                if (!parsed.has_value() || *parsed == TroubleshootMode::Off) {
                    res.status = 400;
                    res.set_content("{\"error\":\"invalid troubleshoot mode\"}", "application/json");
                    return;
                }
                mode = *parsed;
            }
            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"run not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }
            if (run->get_status() != "failed") {
                res.status = 400;
                res.set_content("{\"error\":\"run is not in failed state\"}", "application/json");
                return;
            }
            if (run->logs_root.empty()) {
                res.status = 404;
                res.set_content("{\"error\":\"run has no logs_root\"}", "application/json");
                return;
            }

            const std::string run_dir = run->logs_root;
            const std::string session_id = reserve_troubleshoot_session_id(run_dir);
            // SPRINT-016 B4 fix: each troubleshoot session gets its own
            // NativeProcessRunner so cancel can kill *only* that session's
            // child process, not every child the server has spawned.
            auto runner = std::make_shared<NativeProcessRunner>();
            {
                std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
                auto it = troubleshoot_in_flight_.find(run_id);
                if (it != troubleshoot_in_flight_.end() && it->second.active) {
                    res.status = 409;
                    res.set_content("{\"error\":\"troubleshoot already in flight\"}", "application/json");
                    return;
                }
            }

            auto reserve = RunGuard::try_reserve(run_id);
            if (!reserve) {
                res.status = 409;
                nlohmann::json err;
                err["error"] = "concurrent_session";
                err["run_id"] = run_id;
                res.set_content(err.dump(), "application/json");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
                auto it = troubleshoot_in_flight_.find(run_id);
                if (it != troubleshoot_in_flight_.end() && it->second.active) {
                    res.status = 409;
                    res.set_content("{\"error\":\"troubleshoot already in flight\"}", "application/json");
                    return;
                }
                TroubleshootInFlight inflight;
                inflight.active = true;
                inflight.session_id = session_id;
                inflight.agent_pid = 0;
                inflight.runner = runner;
                troubleshoot_in_flight_[run_id] = inflight;
            }

            // SPRINT-016 M11 fix: store the thread handle so stop() can
            // join it; do not detach.
            try {
                std::lock_guard<std::mutex> lock(troubleshoot_threads_mutex_);
                troubleshoot_threads_.emplace_back([this, run, run_id, run_dir, session_id, mode, runner,
                                                    run_guard = std::move(*reserve)]() mutable {
                    // run_guard's destructor releases the RunGuard slot on worker exit.
                    try {
                        if (troubleshoot_worker_test_hook_) {
                            troubleshoot_worker_test_hook_(run_id, session_id);
                        }
                        JsonCheckpointWriter writer;
                        auto cp_result = writer.load(run_dir + "/checkpoint.json");
                        if (cp_result.ok()) {
                            Checkpoint cp = cp_result.value();
                            std::string project_dir = cp.context.get("needle.project_dir").empty()
                                ? run->project_dir
                                : cp.context.get("needle.project_dir");
                            if (project_dir.empty()) project_dir = ".";
                            std::string graph_path = cp.context.get("needle.graph_path");
                            if (graph_path.empty()) graph_path = cp.graph_file;
                            if (!graph_path.empty() && !platform::is_absolute_path(graph_path)) {
                                graph_path = platform::path_join(project_dir, graph_path);
                            }
                            cp.context.set("needle.project_dir", project_dir);
                            cp.context.set("needle.run_id", run_id);
                            cp.context.set("needle.troubleshoot_session_id", session_id);
                            cp.context.set("needle.run_guard_reserved", "true");
                            if (!graph_path.empty()) cp.context.set("needle.graph_path", graph_path);
                            cp.context.set("needle.logs_root", run_dir);

                            auto graph_result = load_troubleshoot_graph(graph_path);
                            if (graph_result.ok()) {
                                AutoTroubleshoot ats(runner);
                                ats.handle(cp.current_node, graph_result.value(), run_dir, cp.context,
                                           config_.max_attempts_per_stage > 0 ? config_.max_attempts_per_stage : 1,
                                           mode, &run->event_bus);
                            }
                        }
                    } catch (const std::exception& e) {
                        NEEDLE_LOG_ERROR("troubleshoot",
                                         "worker for run=%s session=%s failed: %s",
                                         run_id.c_str(), session_id.c_str(), e.what());
                    } catch (...) {
                        NEEDLE_LOG_ERROR("troubleshoot",
                                         "worker for run=%s session=%s failed with unknown exception",
                                         run_id.c_str(), session_id.c_str());
                    }

                    std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
                    auto it = troubleshoot_in_flight_.find(run_id);
                    if (it != troubleshoot_in_flight_.end() && it->second.session_id == session_id) {
                        it->second.active = false;
                    }
                });
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
                auto it = troubleshoot_in_flight_.find(run_id);
                if (it != troubleshoot_in_flight_.end() && it->second.session_id == session_id) {
                    it->second.active = false;
                }
                res.status = 500;
                nlohmann::json err;
                err["error"] = "failed to start troubleshoot worker";
                err["detail"] = e.what();
                res.set_content(err.dump(), "application/json");
                return;
            }

            nlohmann::json j;
            j["session_id"] = session_id;
            j["status"] = "accepted";
            res.status = 202;
            res.set_content(j.dump(), "application/json");
        });

        svr.Post(R"(/api/v1/runs/([\w.-]+)/troubleshoot/cancel)",
                 [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() ||
                !body.contains("session_id") || !body["session_id"].is_string()) {
                res.status = 400;
                res.set_content("{\"error\":\"session_id is required\"}", "application/json");
                return;
            }
            std::string session_id = body["session_id"].get<std::string>();

            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"run not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }

            bool killed = false;
            {
                std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
                auto it = troubleshoot_in_flight_.find(run_id);
                if (it != troubleshoot_in_flight_.end() && it->second.session_id == session_id &&
                    it->second.active) {
                    // SPRINT-016 B4 fix: the runner stored in troubleshoot_in_flight_
                    // is session-scoped, so kill_all() here is bounded to this
                    // session's child(ren), not the server-wide process pool.
                    if (it->second.runner) {
                        it->second.runner->kill_all();
                        killed = true;
                    }
                    it->second.active = false;
                }
            }
            if (!run->logs_root.empty()) {
                const std::string session_dir = session_id.rfind("session-", 0) == 0
                    ? run->logs_root + "/troubleshoot/" + session_id
                    : run->logs_root + "/troubleshoot/session-" + session_id;
                write_troubleshoot_cancel_marker(session_dir);
            }

            nlohmann::json j;
            j["status"] = "ok";
            j["session_id"] = session_id;
            j["killed"] = killed;
            res.set_content(j.dump(), "application/json");
        });

        svr.Post(R"(/api/v1/runs/([\w.-]+)/troubleshoot/rollback)",
                 [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() ||
                !body.contains("session_id") || !body["session_id"].is_string()) {
                res.status = 400;
                res.set_content("{\"error\":\"session_id is required\"}", "application/json");
                return;
            }
            std::string session_id = body["session_id"].get<std::string>();

            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"run not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }
            if (run->logs_root.empty()) {
                res.status = 404;
                res.set_content("{\"error\":\"run has no logs_root\"}", "application/json");
                return;
            }
            const std::string session_dir = session_id.rfind("session-", 0) == 0
                ? run->logs_root + "/troubleshoot/" + session_id
                : run->logs_root + "/troubleshoot/session-" + session_id;
            if (!platform::is_directory(session_dir)) {
                res.status = 404;
                res.set_content("{\"error\":\"troubleshoot session not found\"}", "application/json");
                return;
            }

            auto result = TroubleshootBackup::rollback(run->project_dir, session_dir);
            if (!result.ok()) {
                res.status = 409;
                nlohmann::json err;
                err["error"] = "rollback_refused";
                err["message"] = result.error();
                res.set_content(err.dump(), "application/json");
                return;
            }
            const auto& rep = result.value();
            nlohmann::json ok;
            ok["status"] = "ok";
            ok["action"] = "rollback";
            ok["session_id"] = session_id;
            ok["backup_branch"] = rep.branch;
            ok["base_sha"] = rep.base_sha;
            ok["untracked_drift"] = rep.untracked_drift;
            ok["reset_pre_modified"] = rep.reset_pre_modified;
            res.set_content(ok.dump(), "application/json");
        });

        // ── /api/v1/browse (GET) ─────────────────────────────────
        svr.Get("/api/v1/browse", [](const httplib::Request& req, httplib::Response& res) {
            std::string path = ".";
            if (req.has_param("path")) {
                path = req.get_param_value("path");
            }

            // Expand ~
            if (!path.empty() && path[0] == '~') {
                std::string home = platform::home_dir();
                if (!home.empty()) path = home + path.substr(1);
            }

            // Resolve to absolute
            if (!platform::is_absolute_path(path)) {
                path = platform::path_join(platform::getcwd_str(), path);
            }

            // Normalize path separators to platform native
#ifdef _WIN32
            for (auto& c : path) { if (c == '/') c = '\\'; }
#else
            // nothing — already using /
#endif

            // Resolve . and .. components
            {
                char sep =
#ifdef _WIN32
                    '\\';
#else
                    '/';
#endif
                std::vector<std::string> parts;
                std::string segment;
                for (size_t i = 0; i < path.size(); ++i) {
                    if (path[i] == '/' || path[i] == '\\') {
                        if (!segment.empty()) {
                            if (segment == "..") {
                                if (!parts.empty()) parts.pop_back();
                            } else if (segment != ".") {
                                parts.push_back(segment);
                            }
                            segment.clear();
                        } else if (i == 0) {
                            // Leading separator (Unix root)
                            parts.push_back("");
                        }
                    } else {
                        segment += path[i];
                    }
                }
                if (!segment.empty()) {
                    if (segment == "..") {
                        if (!parts.empty()) parts.pop_back();
                    } else if (segment != ".") {
                        parts.push_back(segment);
                    }
                }
                // Rebuild path
                path.clear();
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (i > 0) path += sep;
                    path += parts[i];
                }
#ifdef _WIN32
                // Ensure drive letter has trailing backslash: "C:" -> "C:\"
                if (path.size() == 2 && path[1] == ':') path += sep;
#else
                if (path.empty()) path = "/";
#endif
            }

            nlohmann::json j;
            j["path"] = path;
#ifdef _WIN32
            j["sep"] = "\\";
#else
            j["sep"] = "/";
#endif
            j["dirs"] = nlohmann::json::array();

            // Optional filter: ?files=.dot,.gv to also list matching files
            std::string file_filter;
            if (req.has_param("files")) {
                file_filter = req.get_param_value("files");
            }

            j["files"] = nlohmann::json::array();

            auto entries = platform::list_directory(path);
            if (!entries.empty() || platform::is_directory(path)) {
                // Add ".." for navigating up (unless at filesystem root)
#ifdef _WIN32
                bool is_root = (path.size() <= 3 && path.size() >= 2 && path[1] == ':');
#else
                bool is_root = (path == "/");
#endif
                if (!is_root) {
                    j["dirs"].push_back("..");
                }

                for (const auto& name : entries) {
                    if (name == ".") continue;
                    std::string full = platform::path_join(path, name);
                    if (platform::is_directory(full)) {
                        j["dirs"].push_back(name);
                    } else if (!file_filter.empty() && platform::is_regular_file(full)) {
                        // Check if file extension matches filter
                        for (size_t pos = 0; pos < file_filter.size(); ) {
                            size_t comma = file_filter.find(',', pos);
                            std::string ext = file_filter.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                            if (name.size() > ext.size() &&
                                name.substr(name.size() - ext.size()) == ext) {
                                j["files"].push_back(name);
                                break;
                            }
                            if (comma == std::string::npos) break;
                            pos = comma + 1;
                        }
                    }
                }
                // Sort
                std::sort(j["dirs"].begin(), j["dirs"].end());
                std::sort(j["files"].begin(), j["files"].end());
            } else {
                j["error"] = "Cannot open directory";
            }

            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/read-file (GET) ──────────────────────────────
        svr.Get("/api/v1/read-file", [](const httplib::Request& req, httplib::Response& res) {
            if (!req.has_param("path")) {
                res.status = 400;
                res.set_content("{\"error\":\"missing path\"}", "application/json");
                return;
            }
            std::string path = req.get_param_value("path");
            // Expand ~
            if (!path.empty() && path[0] == '~') {
                std::string home = platform::home_dir();
                if (!home.empty()) path = home + path.substr(1);
            }
            std::ifstream f(path);
            if (!f.is_open()) {
                res.status = 404;
                res.set_content("{\"error\":\"file not found\"}", "application/json");
                return;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "text/plain; charset=utf-8");
        });

        // ── /api/v1/write-file (POST) ────────────────────────────
        // Save editor content back to disk so the dashboard can route
        // Run/Resume through dot_path without ever having the server
        // duplicate the file.
        svr.Post("/api/v1/write-file", [](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid JSON\"}", "application/json");
                return;
            }
            if (!body.contains("path") || !body["path"].is_string() ||
                !body.contains("content") || !body["content"].is_string()) {
                res.status = 400;
                res.set_content(
                    "{\"error\":\"path and content are required strings\"}",
                    "application/json");
                return;
            }
            std::string path = body["path"].get<std::string>();
            std::string content = body["content"].get<std::string>();

            if (!path.empty() && path[0] == '~') {
                std::string home = platform::home_dir();
                if (!home.empty()) path = home + path.substr(1);
            }
            if (path.empty()) {
                res.status = 400;
                res.set_content("{\"error\":\"path is empty\"}", "application/json");
                return;
            }

            // Ensure parent directory exists.
            auto slash = path.find_last_of("/\\");
            if (slash != std::string::npos) {
                std::string parent = path.substr(0, slash);
                if (!parent.empty()) platform::mkdir_p(parent);
            }

            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) {
                res.status = 500;
                nlohmann::json err;
                err["error"] = "failed to write " + path;
                res.set_content(err.dump(), "application/json");
                return;
            }
            out << content;
            out.close();

            nlohmann::json j;
            j["path"] = path;
            j["bytes"] = content.size();
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/runs/:id/interactive (GET) ───────────────────
        // Returns the current interactive session state for a run
        svr.Get(R"(/api/v1/runs/([\w.-]+)/interactive)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }

            nlohmann::json j;
            j["active"] = false;

            std::shared_ptr<InteractiveSession> selected_session = run->interactive_session;
            if (req.has_param("node_id")) {
                auto registered = InteractiveSessionRegistry::get(req.get_param_value("node_id"));
                if (registered) selected_session = registered;
            }

            if (selected_session) {
                std::lock_guard<std::mutex> lock(selected_session->mutex);
                if (selected_session->active) {
                    j["active"] = true;
                    j["node_id"] = selected_session->node_id;
                    j["prompt"] = selected_session->prompt;
                    j["context_summary"] = selected_session->context_summary;
                    j["pipeline_context"] = selected_session->pipeline_context;

                    // Include persisted chat history so reconnecting clients
                    // can restore the conversation after a disconnect.
                    if (!run->logs_root.empty()) {
                        std::string history_path = run->logs_root + "/stages/"
                            + selected_session->node_id + "/chat_history.json";
                        std::ifstream in(history_path);
                        if (in.is_open()) {
                            auto hist = nlohmann::json::parse(in, nullptr, false);
                            if (hist.is_array()) {
                                j["chat_history"] = std::move(hist);
                            }
                        }
                        if (!j.contains("chat_history") && !selected_session->opener.empty()) {
                            nlohmann::json hist = nlohmann::json::array();
                            hist.push_back({{"role", "assistant"}, {"content", selected_session->opener}});
                            std::string stage_dir = run->logs_root + "/stages/" + selected_session->node_id;
                            platform::mkdir_p(stage_dir);
                            std::ofstream out(stage_dir + "/chat_history.json");
                            if (out.is_open()) out << hist.dump(2);
                            j["chat_history"] = std::move(hist);
                        }
                    }
                }
            }

            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/runs/:id/interactive/chat (POST) ────────────
        // Chat with the LLM during an interactive session
        svr.Post(R"(/api/v1/runs/([\w.-]+)/interactive/chat)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];

            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }

            // Check that the interactive session is active
            if (!run->interactive_session) {
                res.status = 409;
                res.set_content("{\"error\":\"no interactive session\"}", "application/json");
                return;
            }

            std::string session_prompt;
            std::string session_context;
            std::string session_pipeline_context;
            {
                std::lock_guard<std::mutex> lock(run->interactive_session->mutex);
                if (!run->interactive_session->active) {
                    res.status = 409;
                    res.set_content("{\"error\":\"interactive session not active\"}", "application/json");
                    return;
                }
                session_prompt = run->interactive_session->prompt;
                session_context = run->interactive_session->context_summary;
                session_pipeline_context = run->interactive_session->pipeline_context;
            }

            nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("message")) {
                res.status = 400;
                res.set_content("{\"error\":\"missing message field\"}", "application/json");
                return;
            }

            std::string user_message = body["message"].get<std::string>();

            // Build the full prompt with pipeline awareness and conversation history
            std::string full_prompt =
                "You are a collaborative AI assistant working within an automated pipeline. "
                "Your role in this step is to collaborate with the user to produce a clear deliverable "
                "that feeds into the next pipeline step.\n\n";
            if (!session_pipeline_context.empty()) {
                full_prompt += "## Pipeline Position\n" + session_pipeline_context + "\n"
                    "Your output from this step will be passed directly to the next step. "
                    "Keep this in mind as you help the user refine their ideas.\n\n";
            }
            full_prompt += "## Stage Goal\n" + session_prompt + "\n\n";
            if (!session_context.empty()) {
                full_prompt += "## Context\n" + session_context + "\n\n";
            }
            full_prompt +=
                "Help the user explore, refine, and develop their idea toward a concrete deliverable for the next step. "
                "Ask clarifying questions, suggest improvements, and provide concrete feedback. "
                "Be proactive and specific. When the user is satisfied, they can click Continue to advance the pipeline, "
                "or Go Back to revisit a previous step.\n\n";

            // Append conversation history. Skip error-role entries — those
            // are our own failure markers from prior chat attempts and would
            // confuse the model if replayed as prior turns.
            //
            // Format uses "User:" / "Assistant:" headings (not "**user**:")
            // to avoid the model treating the transcript as a dialog template
            // it should continue by simulating additional turns.
            if (body.contains("history") && body["history"].is_array()) {
                bool any = false;
                for (const auto& m : body["history"]) {
                    std::string role = m.value("role", "user");
                    std::string content = m.value("content", "");
                    if (role == "error" || role == "system") continue;
                    if (role != "user" && role != "assistant") continue;
                    if (content.empty()) continue;
                    if (!any) {
                        full_prompt += "## Conversation so far\n\n"
                                       "(Previous turns, most recent last. Do NOT "
                                       "extend this transcript — only reply to the "
                                       "current user message below.)\n\n";
                        any = true;
                    }
                    std::string label = (role == "user") ? "User" : "Assistant";
                    full_prompt += label + ":\n" + content + "\n\n";
                }
            }

            full_prompt += "## Current user message\n\n" + user_message + "\n\n"
                           "Reply directly to the user's current message. "
                           "Give exactly one reply and stop. Do NOT simulate "
                           "further user or assistant turns. Do NOT prefix "
                           "your reply with 'Assistant:' or any role label. "
                           "Do NOT include any scratchpad, internal-reasoning, "
                           "or thinking blocks in the output.";

            // Use the CLI agent via ProcessRunner
            std::string agent = body.value("agent",
                NeedleConfig::global().get_string("defaults.chat_agent", "", "claude"));
            std::string model = body.value("model",
                NeedleConfig::global().get_string("defaults.chat_model", "", "claude-sonnet-4-6"));

            auto process_runner = std::make_shared<NativeProcessRunner>();
            std::vector<std::string> args;
            if (agent == "codex") {
                args = {"exec", "-m", model, "-s", "danger-full-access"};
            } else if (agent == "gemini") {
                args = {"--yolo", "--model", model, "--prompt"};
            } else {
                // Default: claude
                // Chat uses --output-format json for reliable output extraction
                // (unlike codergen which uses plain text for streaming progress)
                args = {"-p", "--model", model, "--output-format", "json", "--dangerously-skip-permissions"};
            }

            // 5-minute budget: deep chats with large history + expanded
            // context can legitimately take over 2 minutes. Keep the failure
            // message in sync with this value when touching it.
            const int CHAT_TIMEOUT_MS = 300000;
            auto proc_result = process_runner->run(agent, args, ".", CHAT_TIMEOUT_MS, {}, full_prompt);

            nlohmann::json j;
            if (proc_result.ok() && proc_result.value().exit_code == 0) {
                std::string output = proc_result.value().stdout_output;
                // Try to extract text from JSON output format
                try {
                    auto parsed = nlohmann::json::parse(output);
                    if (parsed.count("result")) {
                        output = parsed["result"].get<std::string>();
                    }
                } catch (...) {
                    // Not JSON — use raw output
                }
                j["response"] = output;

                // Persist this chat turn incrementally so the conversation
                // survives crashes, timeouts, or disconnects.
                if (!run->logs_root.empty()) {
                    std::string node_id;
                    {
                        std::lock_guard<std::mutex> lock(run->interactive_session->mutex);
                        node_id = run->interactive_session->node_id;
                    }
                    if (!node_id.empty()) {
                        std::string stage_dir = run->logs_root + "/stages/" + node_id;
                        platform::mkdir_p(stage_dir);
                        std::string history_path = stage_dir + "/chat_history.json";

                        nlohmann::json history = nlohmann::json::array();
                        {
                            std::ifstream in(history_path);
                            if (in.is_open()) {
                                auto parsed_hist = nlohmann::json::parse(in, nullptr, false);
                                if (parsed_hist.is_array()) {
                                    history = std::move(parsed_hist);
                                }
                            }
                        }

                        history.push_back({{"role", "user"}, {"content", user_message}});
                        history.push_back({{"role", "assistant"}, {"content", output}});

                        std::ofstream out(history_path);
                        if (out.is_open()) {
                            out << history.dump(2);
                        }
                    }
                }
            } else {
                // Gather everything useful about the failure so nothing is silent.
                const ProcessResult* pr = proc_result.ok() ? &proc_result.value() : nullptr;
                std::string runner_err = proc_result.ok() ? std::string() : proc_result.error();
                std::string stderr_out = pr ? pr->stderr_output : std::string();
                std::string stdout_out = pr ? pr->stdout_output : std::string();
                int exit_code = pr ? pr->exit_code : -1;
                bool timed_out = pr ? pr->timed_out : false;

                auto rtrim = [](std::string s) {
                    while (!s.empty() &&
                           (s.back() == '\n' || s.back() == '\r' ||
                            s.back() == ' ' || s.back() == '\t')) s.pop_back();
                    return s;
                };
                auto tail = [](const std::string& s, size_t n) {
                    if (s.size() <= n) return s;
                    size_t start = s.size() - n;
                    auto nl = s.find('\n', start);
                    if (nl != std::string::npos) start = nl + 1;
                    return std::string("…") + s.substr(start);
                };

                // Build a rich detail string — prefer stderr, fall back to
                // stdout, fall back to exit code. Never return bare "failed:".
                std::string detail;
                if (timed_out) {
                    detail = "timed out after " +
                             std::to_string(CHAT_TIMEOUT_MS / 1000) + "s";
                } else if (!runner_err.empty()) {
                    detail = runner_err;
                } else if (!rtrim(stderr_out).empty()) {
                    detail = rtrim(stderr_out);
                } else if (!rtrim(stdout_out).empty()) {
                    detail = "exit_code=" + std::to_string(exit_code) +
                             ", no stderr; stdout tail: " +
                             tail(rtrim(stdout_out), 400);
                } else {
                    detail = "exit_code=" + std::to_string(exit_code) +
                             " (no stdout or stderr)";
                }

                NEEDLE_LOG_WARN("server",
                    "interactive chat failed: run=%s agent=%s model=%s "
                    "exit_code=%d timed_out=%d stderr=[%s] stdout_len=%zu "
                    "runner_err=[%s]",
                    run->id.c_str(), agent.c_str(), model.c_str(),
                    exit_code, (int)timed_out,
                    rtrim(stderr_out).c_str(), stdout_out.size(),
                    runner_err.c_str());

                // Persist the failure to chat_history.json so the conversation
                // record shows what happened instead of a silent gap.
                if (!run->logs_root.empty()) {
                    std::string node_id;
                    {
                        std::lock_guard<std::mutex> lock(run->interactive_session->mutex);
                        node_id = run->interactive_session->node_id;
                    }
                    if (!node_id.empty()) {
                        std::string stage_dir = run->logs_root + "/stages/" + node_id;
                        platform::mkdir_p(stage_dir);
                        std::string history_path = stage_dir + "/chat_history.json";

                        nlohmann::json history = nlohmann::json::array();
                        {
                            std::ifstream in(history_path);
                            if (in.is_open()) {
                                auto parsed_hist = nlohmann::json::parse(in, nullptr, false);
                                if (parsed_hist.is_array()) {
                                    history = std::move(parsed_hist);
                                }
                            }
                        }

                        history.push_back({{"role", "user"}, {"content", user_message}});
                        history.push_back({{"role", "error"},
                                           {"content", "Chat agent failed: " + detail}});

                        std::ofstream out(history_path);
                        if (out.is_open()) {
                            out << history.dump(2);
                        }
                    }
                }

                res.status = 502;
                j["error"] = "Chat agent failed: " + detail;
            }

            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/runs/:id/continue (POST) ───────────────────
        // Continues an interactive stage with the provided result
        svr.Post(R"(/api/v1/runs/([\w.-]+)/continue)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];

            nlohmann::json body;
            if (!req.body.empty()) {
                body = nlohmann::json::parse(req.body, nullptr, false);
            }

            std::string result_text;
            std::string action = "continue";
            if (body.is_object()) {
                if (body.contains("result")) {
                    result_text = body["result"].get<std::string>();
                }
                if (body.contains("action")) {
                    action = body["action"].get<std::string>();
                }
            }

            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }

            // Signal the interactive session if active
            if (run->interactive_session) {
                std::lock_guard<std::mutex> lock(run->interactive_session->mutex);
                if (run->interactive_session->active) {
                    run->interactive_session->final_result = result_text;
                    run->interactive_session->go_back = (action == "go_back");
                    run->interactive_session->continued = true;
                    run->interactive_session->cv.notify_one();

                    nlohmann::json j;
                    j["status"] = "continued";
                    res.set_content(j.dump(), "application/json");
                    return;
                }
            }

            // Fallback: route to HttpInterviewer for wait_human gates (M1 fix)
            auto http_iv = std::dynamic_pointer_cast<HttpInterviewer>(run->interviewer);
            if (http_iv) {
                // Parse selected_index from body — never hardcode to 0
                if (!body.is_object() || !body.contains("selected_index")) {
                    res.status = 400;
                    res.set_content("{\"error\":\"no active interactive session and no selected_index provided\"}", "application/json");
                    return;
                }
                nlohmann::json answer;
                answer["selected_index"] = body["selected_index"];
                answer["raw_input"] = result_text;
                http_iv->post_answer(answer.dump());
            }

            nlohmann::json j;
            j["status"] = "continued";
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/stages/:node_id (GET) ───────────────────────
        svr.Get(R"(/api/v1/stages/(\w[\w\-]*))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string node_id = req.matches[1];

            // Find the logs_root: use run-specific if provided, else search
            std::string stage_dir;
            std::string run_id = req.has_param("run_id") ? req.get_param_value("run_id") : "";
            if (!run_id.empty()) {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it != runs_.end() && !it->second->logs_root.empty()) {
                    stage_dir = it->second->logs_root + "/stages/" + node_id;
                }
                // When run_id is specified, use ONLY that run's directory — no fallback
            }
            if (stage_dir.empty() && run_id.empty()) {
                // No run_id specified — search all runs (legacy behavior)
                std::lock_guard<std::mutex> lock(runs_mutex_);
                for (const auto& pair : runs_) {
                    if (!pair.second->logs_root.empty()) {
                        std::string candidate = pair.second->logs_root + "/stages/" + node_id;
                        if (platform::file_exists(candidate)) {
                            stage_dir = candidate;
                            break;
                        }
                    }
                }
            }
            if (stage_dir.empty()) {
                stage_dir = config_.logs_root + "/stages/" + node_id;
            }

            nlohmann::json j;
            j["node_id"] = node_id;

            // Read prompt
            std::ifstream pf(stage_dir + "/prompt.md");
            if (pf.is_open()) {
                std::ostringstream ss;
                ss << pf.rdbuf();
                j["prompt"] = ss.str();
            }

            // Read response
            std::ifstream rf(stage_dir + "/response.md");
            if (rf.is_open()) {
                std::ostringstream ss;
                ss << rf.rdbuf();
                j["response"] = ss.str();
            }

            // Read status.json if present
            std::ifstream sf(stage_dir + "/status.json");
            if (sf.is_open()) {
                try {
                    nlohmann::json status;
                    sf >> status;
                    j["status_detail"] = status;
                } catch (...) {}
            }

            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/runs (POST) ────────────────────────────────
        svr.Post("/api/v1/runs", [this](const httplib::Request& req, httplib::Response& res) {
            nlohmann::json body;
            if (!req.body.empty()) {
                body = nlohmann::json::parse(req.body, nullptr, false);
                if (body.is_discarded()) {
                    res.status = 400;
                    res.set_content("{\"error\":\"invalid JSON\"}", "application/json");
                    return;
                }
            }

            // Extract project_dir and vars from body
            std::string project_dir = ".";
            std::map<std::string, std::string> vars;
            if (body.is_object()) {
                if (body.value("dry_run", false)) {
                    res.status = 400;
                    nlohmann::json err;
                    err["error"] = "dry_run launches are not supported by POST /api/v1/runs";
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                if (body.contains("project_dir") && body["project_dir"].is_string()) {
                    project_dir = body["project_dir"].get<std::string>();
                    // Expand ~ to home directory
                    if (!project_dir.empty() && project_dir[0] == '~') {
                        std::string home = platform::home_dir();
                        if (!home.empty()) {
                            project_dir = home + project_dir.substr(1);
                        }
                    }
                    // Create directory if it doesn't exist
                    if (!platform::file_exists(project_dir)) {
                        platform::mkdir_p(project_dir);
                    }
                }
                if (body.contains("vars") && body["vars"].is_object()) {
                    for (auto it = body["vars"].begin(); it != body["vars"].end(); ++it) {
                        vars[it.key()] = it.value().get<std::string>();
                    }
                }
            }

            // Two ways to supply the DOT:
            //   - dot_path: a file path the caller has already saved.
            //     Server reads from disk; never writes a copy.
            //   - dot_source: raw DOT contents (only legitimate use is
            //     auto-generated DOTs that have no on-disk home yet).
            //     Server stashes ONE canonical copy at
            //     <project_dir>/.needle/<stem>/source.dot — never in the
            //     project root — and uses that as the on-disk source.
            // dot_path wins if both are set.
            std::string dot_path;
            if (body.is_object() && body.contains("dot_path") &&
                body["dot_path"].is_string()) {
                dot_path = body["dot_path"].get<std::string>();
                if (!dot_path.empty() && dot_path[0] == '~') {
                    std::string home = platform::home_dir();
                    if (!home.empty()) dot_path = home + dot_path.substr(1);
                }
                // Resolve relative paths against project_dir so the caller
                // can pass a bare basename when project_dir is set.
                if (!dot_path.empty() && !platform::is_absolute_path(dot_path) &&
                    !project_dir.empty() && project_dir != ".") {
                    dot_path = project_dir + "/" + dot_path;
                }
            }

            std::string dot_source;
            if (body.is_object() && body.contains("dot_source") &&
                body["dot_source"].is_string()) {
                dot_source = body["dot_source"].get<std::string>();
            }

            // dot_path is the canonical source — read it now, overriding
            // any client-supplied dot_source.
            if (!dot_path.empty()) {
                std::string disk = read_file(dot_path);
                if (disk.empty() && !platform::file_exists(dot_path)) {
                    res.status = 400;
                    nlohmann::json err;
                    err["error"] = "Cannot read dot_path: " + dot_path;
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                dot_source = std::move(disk);
            }

            // If the client supplied no DOT at all, fall back to the
            // server's pre-loaded graph (the CLI `serve` mode boots with
            // a graph; tests rely on this path too).
            Graph run_graph = graph_;
            if (!dot_source.empty()) {
                DotParser parser(dot_source);
                auto parse_result = parser.parse();
                if (!parse_result.ok()) {
                    res.status = 400;
                    nlohmann::json err;
                    err["error"] = parse_result.error();
                    res.set_content(err.dump(), "application/json");
                    return;
                }

                GraphBuilder builder;
                auto build_result = builder.build(parse_result.value());
                if (!build_result.ok()) {
                    res.status = 400;
                    nlohmann::json err;
                    err["error"] = build_result.error();
                    res.set_content(err.dump(), "application/json");
                    return;
                }

                run_graph = build_result.value();

                auto style_result = apply_configured_stylesheets(run_graph, config_.stylesheet_file);
                if (!style_result.ok()) {
                    res.status = 400;
                    nlohmann::json err;
                    err["error"] = style_result.error();
                    res.set_content(err.dump(), "application/json");
                    return;
                }

                auto validator = GraphValidator::create_default();
                auto diags = validator.validate(run_graph);
                if (diags.has_errors()) {
                    res.status = 400;
                    nlohmann::json err;
                    err["error"] = "validation failed";
                    nlohmann::json details = nlohmann::json::array();
                    for (const auto& d : diags.errors()) {
                        details.push_back(d.message);
                    }
                    err["details"] = details;
                    res.set_content(err.dump(), "application/json");
                    return;
                }
            }

            // Resolve the canonical on-disk path. With dot_path that's the
            // path we just read. Without it, stash dot_source under
            // .needle/<stem>/source.dot so there's exactly one on-disk
            // copy and it's namespaced to the run, not the project root.
            //
            // Either way, also write a frozen snapshot to
            // <logs_root>/source.dot so `resume --from-snapshot` can
            // recover the original content even if the user edited
            // dot_path on disk in the meantime.
            std::string canonical_dot_path;
            std::string stem_override;
            if (!dot_path.empty()) {
                canonical_dot_path = dot_path;
                stem_override = dot_stem_from_filename(dot_path);
            } else if (!project_dir.empty() && project_dir != "." &&
                       platform::is_directory(project_dir)) {
                std::string stem = dot_stem_from_source(dot_source);
                std::string stash_dir = compute_logs_root(project_dir, stem);
                platform::mkdir_p(stash_dir);
                std::string target = stash_dir + "/source.dot";
                if (read_file(target) != dot_source) {
                    std::ofstream out(target, std::ios::binary);
                    if (!out.is_open()) {
                        NEEDLE_LOG_WARN("server",
                            "failed to write dot to %s", target.c_str());
                        target.clear();
                    } else {
                        out << dot_source;
                    }
                }
                canonical_dot_path = target;
                stem_override = stem;
            }
            canonical_dot_path = absolute_path(canonical_dot_path);
            // Always write the frozen snapshot under logs_root for path-
            // backed runs. The stash branch above already did this for
            // inline-source runs (compute_logs_root == stash_dir).
            if (!dot_path.empty() && !project_dir.empty() && project_dir != "." &&
                platform::is_directory(project_dir) && !stem_override.empty()) {
                std::string snap_dir = compute_logs_root(project_dir, stem_override);
                platform::mkdir_p(snap_dir);
                std::string snap_target = snap_dir + "/source.dot";
                if (read_file(snap_target) != dot_source) {
                    std::ofstream out(snap_target, std::ios::binary);
                    if (out.is_open()) out << dot_source;
                }
            }

            auto run = create_run(run_graph, dot_source, project_dir, vars,
                                  stem_override, canonical_dot_path);

            nlohmann::json j;
            j["id"] = run->id;
            j["status"] = "running";
            if (!canonical_dot_path.empty()) j["dot_path"] = canonical_dot_path;
            res.status = 201;
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/resume (POST) ────────────────────────────
        // Resume a pipeline from a checkpoint in a project directory
        svr.Post("/api/v1/resume", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded()) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid JSON\"}", "application/json");
                return;
            }

            std::string project_dir = ".";
            if (body.contains("project_dir") && body["project_dir"].is_string()) {
                project_dir = body["project_dir"].get<std::string>();
                if (!project_dir.empty() && project_dir[0] == '~') {
                    std::string home = platform::home_dir();
                    if (!home.empty()) project_dir = home + project_dir.substr(1);
                }
            }

            // Optional explicit DOT path. When set, this is the source of
            // truth for resume — we don't fall back to checkpoint paths
            // and we don't accept inline dot_source content.
            std::string dot_path;
            if (body.contains("dot_path") && body["dot_path"].is_string()) {
                dot_path = body["dot_path"].get<std::string>();
                if (!dot_path.empty() && dot_path[0] == '~') {
                    std::string home = platform::home_dir();
                    if (!home.empty()) dot_path = home + dot_path.substr(1);
                }
                if (!dot_path.empty() && !platform::is_absolute_path(dot_path) &&
                    !project_dir.empty() && project_dir != ".") {
                    dot_path = project_dir + "/" + dot_path;
                }
            }

            // Determine dot_stem for per-DOT checkpoint path.
            // Runs started from inline dot_source land in a label-derived
            // stem dir (dot_stem_from_source), so prefer source-derivation
            // over filename-derivation when both are available. An empty
            // body.dot_stem is treated as missing — the dashboard's in-memory
            // run state may not have it populated on the resume codepath.
            std::string dot_stem;
            if (body.contains("dot_stem") && body["dot_stem"].is_string() &&
                !body["dot_stem"].get<std::string>().empty()) {
                dot_stem = body["dot_stem"].get<std::string>();
            } else if (body.contains("dot_source") && body["dot_source"].is_string() &&
                       !body["dot_source"].get<std::string>().empty()) {
                dot_stem = dot_stem_from_source(body["dot_source"].get<std::string>());
            } else if (!dot_path.empty()) {
                dot_stem = dot_stem_from_filename(dot_path);
            }

            // Try per-DOT path first. If the chosen stem doesn't resolve to
            // an existing checkpoint and we have a dot_path on disk, read it
            // and retry with a source-derived stem — covers the "resume from
            // a run that was created with inline dot_source" case where the
            // logs dir is label-derived but only filename-derivation was
            // available here. Fall back to flat .needle/ as a last resort.
            std::string checkpoint_path;
            if (!dot_stem.empty()) {
                checkpoint_path = compute_logs_root(project_dir, dot_stem) + "/checkpoint.json";
                if (!platform::file_exists(checkpoint_path) && !dot_path.empty()) {
                    std::ifstream alt_in(dot_path);
                    if (alt_in.is_open()) {
                        std::ostringstream alt_ss;
                        alt_ss << alt_in.rdbuf();
                        std::string alt_stem = dot_stem_from_source(alt_ss.str());
                        if (!alt_stem.empty() && alt_stem != dot_stem) {
                            std::string alt_path = compute_logs_root(project_dir, alt_stem) + "/checkpoint.json";
                            if (platform::file_exists(alt_path)) {
                                checkpoint_path = alt_path;
                                dot_stem = alt_stem;
                            }
                        }
                    }
                }
                if (!platform::file_exists(checkpoint_path)) {
                    checkpoint_path = project_dir + "/.needle/checkpoint.json";
                }
            } else {
                checkpoint_path = project_dir + "/.needle/checkpoint.json";
            }

            // Load checkpoint
            JsonCheckpointWriter cp_writer;
            auto cp_result = cp_writer.load(checkpoint_path);
            if (!cp_result.ok()) {
                res.status = 400;
                nlohmann::json err;
                err["error"] = "Failed to load checkpoint: " + cp_result.error();
                res.set_content(err.dump(), "application/json");
                return;
            }
            Checkpoint cp = cp_result.value();

            // Read the graph DOT source. Priority:
            // 1. dot_path provided in the request body (frontend's loaded file)
            // 2. dot_source provided in the request body (legacy fallback)
            // 3. graph_file from the checkpoint (absolute path)
            // 4. graph_file basename in the project directory
            // 5. <project_dir>/.needle/<stem>/source.dot (the canonical
            //    stash path written by POST /api/v1/runs for inline DOTs)
            std::string dot_source;
            if (!dot_path.empty()) {
                std::ifstream f(dot_path);
                if (!f.is_open()) {
                    res.status = 400;
                    nlohmann::json err;
                    err["error"] = "Cannot read dot_path: " + dot_path;
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                std::ostringstream ss;
                ss << f.rdbuf();
                dot_source = ss.str();
            } else if (body.contains("dot_source") && body["dot_source"].is_string() &&
                !body["dot_source"].get<std::string>().empty()) {
                dot_source = body["dot_source"].get<std::string>();
            } else {
                std::string graph_file = cp.graph_file;
                std::ifstream f(graph_file);
                std::vector<std::string> tried_paths;
                if (!graph_file.empty()) tried_paths.push_back(graph_file);

                if (!f.is_open() && !project_dir.empty() && !graph_file.empty()) {
                    // Try basename in project directory
                    std::string basename = graph_file;
                    auto slash = basename.rfind('/');
                    if (slash != std::string::npos) basename = basename.substr(slash + 1);
                    graph_file = project_dir + "/" + basename;
                    tried_paths.push_back(graph_file);
                    f.open(graph_file);
                }
                if (!f.is_open() && !project_dir.empty() && !dot_stem.empty()) {
                    // Fall back to the canonical stash written by
                    // POST /api/v1/runs for inline (dot_source) runs.
                    graph_file = compute_logs_root(project_dir, dot_stem) + "/source.dot";
                    tried_paths.push_back(graph_file);
                    f.open(graph_file);
                }
                if (!f.is_open()) {
                    res.status = 400;
                    nlohmann::json err;
                    std::string tried = tried_paths.empty() ? "(none)" : "";
                    for (size_t i = 0; i < tried_paths.size(); ++i) {
                        if (i) tried += ", ";
                        tried += tried_paths[i];
                    }
                    err["error"] = "Cannot read graph file (tried: " + tried +
                                   "). Load the DOT file in the editor and try again.";
                    res.set_content(err.dump(), "application/json");
                    return;
                }
                std::ostringstream ss;
                ss << f.rdbuf();
                dot_source = ss.str();
            }

            // SPRINT-013 §3.4: detect on-disk edits since run start. The
            // checkpoint records the content hash at run-start time; if
            // the current source differs and the caller didn't pass
            // `reload=true`, return 409 with both hashes so the dashboard
            // can prompt the operator: reload from disk (accept new) or
            // continue from snapshot (refuse the reload, run from .needle
            // stash if available).
            std::string current_hash = std::to_string(std::hash<std::string>{}(dot_source));
            bool reload_requested = body.is_object() && body.value("reload", false);
            if (!cp.dot_content_hash.empty() &&
                cp.dot_content_hash != current_hash &&
                !reload_requested) {
                // If `continue_from_snapshot` is set, try to read the
                // snapshot at <logs_root>/source.dot and run from that.
                bool continue_from_snapshot = body.is_object() &&
                                              body.value("continue_from_snapshot", false);
                if (continue_from_snapshot && !cp.logs_root.empty()) {
                    std::ifstream snap(cp.logs_root + "/source.dot");
                    if (snap.is_open()) {
                        std::ostringstream ss;
                        ss << snap.rdbuf();
                        dot_source = ss.str();
                        current_hash = std::to_string(std::hash<std::string>{}(dot_source));
                    }
                }
                if (cp.dot_content_hash != current_hash) {
                    res.status = 409;
                    nlohmann::json err;
                    err["error"] = "dot_changed";
                    err["snapshot_hash"] = cp.dot_content_hash;
                    err["current_hash"] = current_hash;
                    err["message"] = "The DOT file on disk has changed since this run started.";
                    res.set_content(err.dump(), "application/json");
                    return;
                }
            }
            // Either hashes match, the caller opted in to reload, or this
            // is a legacy checkpoint with no hash. Propagate the
            // (possibly updated) hash so future saves stay in sync.
            cp.dot_content_hash = current_hash;

            // Parse and build graph
            DotParser parser(dot_source);
            auto parse_result = parser.parse();
            if (!parse_result.ok()) {
                res.status = 400;
                nlohmann::json err;
                err["error"] = "Parse error: " + parse_result.error();
                res.set_content(err.dump(), "application/json");
                return;
            }

            GraphBuilder builder;
            auto build_result = builder.build(parse_result.value());
            if (!build_result.ok()) {
                res.status = 400;
                nlohmann::json err;
                err["error"] = "Build error: " + build_result.error();
                res.set_content(err.dump(), "application/json");
                return;
            }
            Graph run_graph = build_result.value();

            auto style_result = apply_configured_stylesheets(run_graph, cp.stylesheet_file);
            if (!style_result.ok()) {
                res.status = 400;
                nlohmann::json err;
                err["error"] = style_result.error();
                res.set_content(err.dump(), "application/json");
                return;
            }

            // Remove old run if replace_run_id was provided
            if (body.contains("replace_run_id") && body["replace_run_id"].is_string()) {
                std::string old_id = body["replace_run_id"].get<std::string>();
                std::shared_ptr<PipelineRun> old_run;
                {
                    std::lock_guard<std::mutex> lock(runs_mutex_);
                    auto it = runs_.find(old_id);
                    if (it != runs_.end()) {
                        old_run = it->second;
                        // Cancel if still running
                        if (old_run->get_status() == "running") {
                            old_run->cancelled.store(true);
                            // Wake any interactive CV so the thread can exit
                            if (old_run->interactive_session) {
                                std::lock_guard<std::mutex> sl(old_run->interactive_session->mutex);
                                old_run->interactive_session->cv.notify_all();
                            }
                        }
                        runs_.erase(it);
                    }
                }
                if (old_run && old_run->run_thread.joinable()) {
                    old_run->run_thread.join();
                }
                run_registry_->remove(old_id);
                run_registry_->save();
            }

            // Create run and resume in background thread
            std::string run_id = generate_run_id(project_dir);
            auto run = std::make_shared<PipelineRun>();
            run->id = run_id;
            run->dot_source = dot_source;
            run->set_status("running");

            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                runs_[run_id] = run;
            }
            active_runs_.fetch_add(1);

            auto run_ptr = run;
            auto gq = global_queue_;
            run->event_bus.subscribe([run_ptr, gq](const PipelineEvent& e) {
                run_ptr->collector.record(e);
                {
                    std::lock_guard<std::mutex> lock(gq->mutex);
                    std::string data = format_pipeline_sse(run_ptr->id, e, gq->sequence++);
                    gq->events.push_back(std::move(data));
                }
            });

            if (!observers_.empty()) {
                std::string observed_run_id = run_id;
                run->event_bus.subscribe([this, observed_run_id](const PipelineEvent& e) {
                    notify_run_event(observed_run_id, e);
                });
            }

            auto http_interviewer = std::make_shared<HttpInterviewer>();
            run->interviewer = http_interviewer;
            run->interactive_session = std::make_shared<InteractiveSession>();

            PipelineConfig config_copy = config_;
            config_copy.interactive_session = run->interactive_session;
            if (config_copy.handler_registry) {
                auto registry_copy = std::make_shared<HandlerRegistry>(*config_copy.handler_registry);
                registry_copy->register_handler("wait_human", make_wait_human_handler(http_interviewer));
                registry_copy->register_handler("interactive",
                    make_interactive_handler(config_copy.cli_backend, run->interactive_session));
                config_copy.handler_registry = registry_copy;
            }

            // Derive stem if we don't have it yet
            if (dot_stem.empty()) {
                dot_stem = dot_stem_from_source(dot_source);
            }
            run->dot_stem = dot_stem;
            run->project_dir = project_dir;
            run->created_at = utc_timestamp_now();

            // Migrate flat .needle/ if needed
            migrate_flat_needle_dir(project_dir, dot_stem);

            config_copy.project_dir = project_dir;
            config_copy.logs_root = compute_logs_root(project_dir, dot_stem);
            // Prefer an explicit dot_path the caller supplied — it's the
            // authoritative on-disk source. Else keep the checkpoint's
            // recorded graph_file so future resumes still find the DOT.
            config_copy.graph_file = absolute_path(!dot_path.empty() ? dot_path : cp.graph_file);
            config_copy.troubleshoot_mode = resolve_troubleshoot_mode(run_graph);
            config_copy.auto_troubleshoot = config_copy.troubleshoot_mode != TroubleshootMode::Off;
            config_copy.troubleshoot_register_runner =
                [this](const std::string& run_id,
                       const std::string& session_id,
                       std::shared_ptr<ProcessRunner> runner) {
                    std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
                    auto it = troubleshoot_in_flight_.find(run_id);
                    if (it != troubleshoot_in_flight_.end() && it->second.active &&
                        it->second.session_id != session_id) {
                        NEEDLE_LOG_WARN("troubleshoot",
                                        "runner registration ignored for run=%s session=%s; active session=%s",
                                        run_id.c_str(), session_id.c_str(),
                                        it->second.session_id.c_str());
                        return;
                    }
                    TroubleshootInFlight& inflight = troubleshoot_in_flight_[run_id];
                    inflight.active = true;
                    inflight.session_id = session_id;
                    inflight.agent_pid = 0;
                    inflight.runner = std::move(runner);
                };
            config_copy.dot_content_hash = cp.dot_content_hash;
            config_copy.checkpoint_writer = std::make_shared<JsonCheckpointWriter>();
            needle::mkdir_p(config_copy.logs_root);
            run->logs_root = config_copy.logs_root;

            // Update checkpoint's logs_root to new per-DOT path
            cp.logs_root = config_copy.logs_root;

            // Persist to registry
            run_registry_->add_entry(run->id, run->dot_stem, run->dot_source,
                                 run->project_dir, run->logs_root,
                                 run->get_status(), run->created_at, run->dry_run);
            run_registry_->save();

            Checkpoint cp_copy = cp;
            cp_copy.context.set("needle.run_id", run_ptr->id);
            if (!config_copy.graph_file.empty()) {
                cp_copy.context.set("needle.graph_path", config_copy.graph_file);
            }
            if (!config_copy.logs_root.empty()) {
                cp_copy.context.set("needle.logs_root", config_copy.logs_root);
                cp_copy.context.set("needle.logs_dir", config_copy.logs_root + "/logs");
                platform::mkdir_p(config_copy.logs_root + "/logs");
            }
            if (!dot_stem.empty()) {
                cp_copy.context.set("needle.dot_stem", dot_stem);
            }
            bool frozen = body.value("frozen_config", false);
            inject_config_defaults(cp_copy.context, NeedleConfig::global(), !frozen);
            auto registry = run_registry_;
            run->run_thread = std::thread([this, run_ptr, run_graph, config_copy, cp_copy, registry]() mutable {
                // M9: Use run_ptr->cancelled so HTTP cancel endpoint controls the engine loop
                PipelineEngine engine(std::move(config_copy), run_ptr->cancelled);
                auto result = engine.resume(cp_copy, run_graph, run_ptr->event_bus);
                std::string status;
                std::string error;
                if (result.ok()) {
                    status = "completed";
                } else {
                    status = "failed";
                    error = result.error();
                }
                run_ptr->set_status(status);
                if (!error.empty()) run_ptr->set_error(error);
                registry->update_status(run_ptr->id, status, error);
                registry->save();
                {
                    std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
                    auto it = troubleshoot_in_flight_.find(run_ptr->id);
                    if (it != troubleshoot_in_flight_.end()) {
                        it->second.active = false;
                        it->second.runner.reset();
                    }
                }
                if (active_runs_.fetch_sub(1) == 1) {
                    notify_idle();
                }
            });
            // M9: Do NOT detach — thread is joined in stop() or PipelineRun destructor

            nlohmann::json j;
            j["id"] = run_id;
            j["status"] = "running";
            j["resumed_from"] = cp.current_node;
            j["dot_source"] = dot_source;
            res.status = 201;
            res.set_content(j.dump(), "application/json");
        });

        // ── DELETE /api/v1/runs/:id ───────────────────────────
        svr.Delete(R"(/api/v1/runs/([\w.-]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            bool delete_artifacts = req.has_param("artifacts") &&
                                    req.get_param_value("artifacts") == "true";

            std::string logs_root;
            std::shared_ptr<PipelineRun> run_to_delete;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"not found\"}", "application/json");
                    return;
                }
                if (it->second->get_status() == "running") {
                    res.status = 409;
                    res.set_content("{\"error\":\"cannot delete running run\"}", "application/json");
                    return;
                }
                run_to_delete = it->second;
                logs_root = it->second->logs_root;
                runs_.erase(it);
            }
            // M9: Join the thread before cleaning up (if completed but thread still joinable)
            if (run_to_delete && run_to_delete->run_thread.joinable()) {
                run_to_delete->run_thread.join();
            }

            run_registry_->remove(run_id);
            run_registry_->save();

            if (delete_artifacts && !logs_root.empty()) {
                platform::remove_recursive(logs_root);
                NEEDLE_LOG_INFO("server", "deleted artifacts for run %s at %s",
                                run_id.c_str(), logs_root.c_str());
            }

            nlohmann::json j;
            j["status"] = "deleted";
            j["artifacts_deleted"] = delete_artifacts;
            res.set_content(j.dump(), "application/json");
        });

        // ── POST /api/v1/check-run ──────────────────────────────
        svr.Post("/api/v1/check-run", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded()) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid JSON\"}", "application/json");
                return;
            }
            std::string project_dir = body.value("project_dir", "");
            std::string dot_source = body.value("dot_source", "");

            // Expand ~
            if (!project_dir.empty() && project_dir[0] == '~') {
                std::string home = platform::home_dir();
                if (!home.empty()) project_dir = home + project_dir.substr(1);
            }

            std::string stem = dot_stem_from_source(dot_source);
            std::string logs_root = compute_logs_root(project_dir, stem);

            nlohmann::json result;
            result["dot_stem"] = stem;
            result["logs_root"] = logs_root;

            // Check for checkpoint (per-DOT path first, then flat fallback)
            std::string cp_path = logs_root + "/checkpoint.json";
            bool has_cp = platform::file_exists(cp_path);
            if (!has_cp) {
                cp_path = project_dir + "/.needle/checkpoint.json";
                has_cp = platform::file_exists(cp_path);
            }
            result["has_checkpoint"] = has_cp;

            // Check for matching run in registry.
            // Identity is (stem, project_dir, content_hash) — not just
            // (stem, project_dir). Without the hash, two DOTs with the same
            // `label=` slug (e.g. user edited the file to fix a bug, kept
            // the title) collide and the dashboard pops up the stale tab
            // instead of treating the edited file as a new run.
            std::string prev_run_id;
            std::size_t incoming_hash = std::hash<std::string>{}(dot_source);
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                for (const auto& pair : runs_) {
                    if (pair.second->dot_stem != stem) continue;
                    if (pair.second->project_dir != project_dir) continue;
                    std::size_t prev_hash = std::hash<std::string>{}(pair.second->dot_source);
                    if (prev_hash != incoming_hash) continue;
                    prev_run_id = pair.first;
                    break;
                }
            }
            result["previous_run_id"] = prev_run_id;
            result["has_previous_run"] = !prev_run_id.empty();

            // If checkpoint exists but no run in registry, reconstruct status from disk
            if (has_cp && prev_run_id.empty()) {
                PipelineRun tmp_run;
                tmp_run.id = "check";
                tmp_run.dot_source = dot_source;
                tmp_run.dot_stem = stem;
                tmp_run.project_dir = project_dir;
                tmp_run.logs_root = logs_root;
                tmp_run.set_status("failed");
                auto view = reconstruct_run_view_from_disk(tmp_run);
                result["node_statuses"] = view.value("node_statuses", nlohmann::json::object());
                result["completed_stages"] = view.value("completed_stages", 0);
                result["total_stages"] = view.value("total_stages", 0);
            }

            res.set_content(result.dump(), "application/json");
        });

        // ── /api/v1/runs/:id/cancel (POST) ────────────────────
        svr.Post(R"(/api/v1/runs/([\w.-]+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            std::lock_guard<std::mutex> lock(runs_mutex_);
            auto it = runs_.find(run_id);
            if (it == runs_.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"not found\"}", "application/json");
                return;
            }
            it->second->cancelled.store(true);
            it->second->set_status("cancelled");
            // Wake the interactive CV so a paused interactive node observes
            // cancellation and unwinds cleanly.
            if (it->second->interactive_session) {
                std::lock_guard<std::mutex> sl(it->second->interactive_session->mutex);
                it->second->interactive_session->cv.notify_all();
            }

            // Kill running child processes immediately so claude sessions
            // don't linger as orphans
            if (config_.process_runner) {
                auto* posix_runner = dynamic_cast<NativeProcessRunner*>(config_.process_runner.get());
                if (posix_runner) {
                    posix_runner->kill_all();
                }
            }

            nlohmann::json j;
            j["id"] = run_id;
            j["status"] = "cancelled";
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/pause (POST) ──────────────────────────────
        // Global pause — all running pipelines stop before their next node
        svr.Post("/api/v1/pause", [this](const httplib::Request&, httplib::Response& res) {
            pause_controller_->pause();
            NEEDLE_LOG_INFO("server", "global pause activated");

            // Emit PIPELINE_PAUSED to global SSE
            {
                PipelineEvent e;
                e.type = EventType::PIPELINE_PAUSED;
                e.timestamp = utc_timestamp_now();
                e.message = "Pipeline execution paused";
                std::lock_guard<std::mutex> lock(global_queue_->mutex);
                global_queue_->events.push_back(format_pipeline_sse("", e));
                global_queue_->sequence++;
            }

            nlohmann::json j;
            j["paused"] = true;
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/pause/resume (POST) ──────────────────────
        // Resume all pipelines — cancel any scheduled resume
        svr.Post("/api/v1/pause/resume", [this](const httplib::Request&, httplib::Response& res) {
            pause_controller_->resume();
            NEEDLE_LOG_INFO("server", "global pause cancelled — resuming");

            // Emit PIPELINE_RESUMED to global SSE
            {
                PipelineEvent e;
                e.type = EventType::PIPELINE_RESUMED;
                e.timestamp = utc_timestamp_now();
                e.message = "Pipeline execution resumed";
                std::lock_guard<std::mutex> lock(global_queue_->mutex);
                global_queue_->events.push_back(format_pipeline_sse("", e));
                global_queue_->sequence++;
            }

            nlohmann::json j;
            j["paused"] = false;
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/pause/schedule (POST) ─────────────────────
        // Schedule resume at a specific time: {"resume_at": "2026-04-01T14:30:00Z"}
        svr.Post("/api/v1/pause/schedule", [this](const httplib::Request& req, httplib::Response& res) {
            nlohmann::json body;
            if (!req.body.empty()) {
                body = nlohmann::json::parse(req.body, nullptr, false);
            }
            if (!body.is_object() || !body.contains("resume_at")) {
                res.status = 400;
                res.set_content("{\"error\":\"missing resume_at\"}", "application/json");
                return;
            }
            std::string resume_at = body["resume_at"].get<std::string>();
            pause_controller_->schedule_resume(resume_at);
            NEEDLE_LOG_INFO("server", "scheduled resume at %s", resume_at.c_str());

            // Launch a timer thread that waits until the scheduled time, then resumes
            // (stop any previous timer first)
            if (pause_timer_thread_.joinable()) {
                pause_controller_->resume();  // wake previous timer's wait
                pause_timer_thread_.join();
            }
            auto pc = pause_controller_;
            auto gq = global_queue_;
            pause_timer_thread_ = std::thread([pc, gq, resume_at]() {
                // Parse ISO 8601 time and sleep until then
                // Simple approach: parse "YYYY-MM-DDTHH:MM:SS" or "YYYY-MM-DDTHH:MM"
                struct tm tm_target = {};
                if (sscanf(resume_at.c_str(), "%d-%d-%dT%d:%d:%d",
                           &tm_target.tm_year, &tm_target.tm_mon, &tm_target.tm_mday,
                           &tm_target.tm_hour, &tm_target.tm_min, &tm_target.tm_sec) < 5) {
                    return;  // invalid format
                }
                tm_target.tm_year -= 1900;
                tm_target.tm_mon -= 1;
                tm_target.tm_isdst = -1;
                time_t target = mktime(&tm_target);
                if (target == (time_t)-1) return;

                // Sleep in 1-second intervals, checking if still paused + scheduled
                while (true) {
                    time_t now = time(nullptr);
                    if (now >= target) break;
                    if (!pc->is_paused()) return;  // manually resumed, abort timer
                    if (pc->get_resume_at() != resume_at) return;  // rescheduled
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }

                // Time reached — resume if still paused with this schedule
                if (pc->is_paused() && pc->get_resume_at() == resume_at) {
                    pc->resume();

                    PipelineEvent e;
                    e.type = EventType::PIPELINE_RESUMED;
                    e.timestamp = utc_timestamp_now();
                    e.message = "Pipeline execution resumed (scheduled)";
                    std::lock_guard<std::mutex> lock(gq->mutex);
                    gq->events.push_back(format_pipeline_sse("", e));
                    gq->sequence++;
                }
            });

            nlohmann::json j;
            j["paused"] = true;
            j["resume_at"] = resume_at;
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/pause/status (GET) ────────────────────────
        svr.Get("/api/v1/pause/status", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j;
            j["paused"] = pause_controller_->is_paused();
            std::string ra = pause_controller_->get_resume_at();
            if (!ra.empty()) j["resume_at"] = ra;
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/runs/:id/answer (POST) ────────────────────
        svr.Post(R"(/api/v1/runs/([\w.-]+)/answer)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];

            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }

            // Route answer to the run's HttpInterviewer
            auto http_iv = std::dynamic_pointer_cast<HttpInterviewer>(run->interviewer);
            if (http_iv) {
                http_iv->post_answer(req.body);
            }

            nlohmann::json j;
            j["status"] = "accepted";
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/graph/svg (GET) ───────────────────────────
        svr.Get("/api/v1/graph/svg", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            if (cached_svg_.empty()) {
                res.set_content("", "image/svg+xml");
            } else {
                res.set_content(cached_svg_, "image/svg+xml");
            }
        });

        // ── /api/v1/graph/dot (GET) ───────────────────────────
        svr.Get("/api/v1/graph/dot", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_content(cached_dot_, "text/plain; charset=utf-8");
        });

        // ── /api/v1/render-dot (POST) ────────────────────────
        // Client-side graph rendering: DOT source → SVG
        svr.Post("/api/v1/render-dot", [](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.contains("dot") || !body["dot"].is_string()) {
                res.status = 400;
                res.set_content("{\"error\":\"missing dot field\"}", "application/json");
                return;
            }
            std::string dot_source = body["dot"].get<std::string>();
            std::string svg = dot_to_svg(dot_source);
            if (svg.empty()) {
                res.status = 500;
                nlohmann::json err;
                err["error"] = "Graphviz rendering failed (is 'dot' installed?)";
                res.set_content(err.dump(), "application/json");
                return;
            }
            nlohmann::json result;
            result["svg"] = svg;
            res.set_content(result.dump(), "application/json");
        });

        // ── /api/v1/logs/needle (GET) ────────────────────────
        // Tail the needle log file for the dashboard logs view
        svr.Get("/api/v1/logs/needle", [](const httplib::Request& req, httplib::Response& res) {
            std::string log_path;
            const char* home = std::getenv("HOME");
            if (home) log_path = std::string(home) + "/.needle/needle.log";

            if (log_path.empty() || !platform::file_exists(log_path)) {
                nlohmann::json j;
                j["lines"] = nlohmann::json::array();
                j["offset"] = 0;
                res.set_content(j.dump(), "application/json");
                return;
            }

            long offset = 0;
            if (req.has_param("offset")) {
                try { offset = std::stol(req.get_param_value("offset")); }
                catch (...) {}
            }

            std::ifstream f(log_path, std::ios::ate);
            if (!f.is_open()) {
                nlohmann::json j;
                j["lines"] = nlohmann::json::array();
                j["offset"] = 0;
                res.set_content(j.dump(), "application/json");
                return;
            }

            long file_size = static_cast<long>(f.tellg());

            // On first request (offset=0), read last 200 lines
            if (offset <= 0) {
                long seek_back = std::min(file_size, 100000L);
                f.seekg(file_size - seek_back);
                std::vector<std::string> lines;
                std::string line;
                if (seek_back < file_size) std::getline(f, line); // skip partial
                while (std::getline(f, line)) lines.push_back(line);
                if (lines.size() > 200) lines.erase(lines.begin(), lines.end() - 200);
                nlohmann::json j;
                j["lines"] = lines;
                j["offset"] = file_size;
                res.set_content(j.dump(), "application/json");
                return;
            }

            // Incremental: read from offset
            if (offset >= file_size) {
                nlohmann::json j;
                j["lines"] = nlohmann::json::array();
                j["offset"] = file_size;
                res.set_content(j.dump(), "application/json");
                return;
            }

            f.seekg(offset);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(f, line)) lines.push_back(line);
            nlohmann::json j;
            j["lines"] = lines;
            j["offset"] = file_size;
            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/check-checkpoint (POST) ─────────────────
        // Check if a .needle/checkpoint.json exists in the given directory
        svr.Post("/api/v1/check-checkpoint", [](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.contains("project_dir") || !body["project_dir"].is_string()) {
                res.status = 400;
                res.set_content("{\"error\":\"missing project_dir\"}", "application/json");
                return;
            }
            std::string project_dir = body["project_dir"].get<std::string>();
            // Expand ~
            if (!project_dir.empty() && project_dir[0] == '~') {
                std::string home = platform::home_dir();
                if (!home.empty()) project_dir = home + project_dir.substr(1);
            }
            std::string checkpoint_path = project_dir + "/.needle/checkpoint.json";
            nlohmann::json result;
            result["exists"] = platform::file_exists(checkpoint_path);
            result["path"] = checkpoint_path;
            res.set_content(result.dump(), "application/json");
        });

        // ── /api/v1/templates (GET) ─────────────────────────
        svr.Get("/api/v1/templates", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            // Scan sample_dots/ for .dot files with template="true" graph attribute
            nlohmann::json templates = nlohmann::json::array();
            auto dir_result = config_.resource_locator.find_dir("sample_dots");
            std::string templates_dir = dir_result.ok() ? dir_result.value() : "sample_dots";

            auto dir_entries = platform::list_directory(templates_dir);
            if (!dir_entries.empty()) {
                for (const auto& name : dir_entries) {
                    if (name.size() < 4 || name.substr(name.size() - 4) != ".dot") continue;

                    std::string path = templates_dir + "/" + name;
                    std::ifstream f(path);
                    if (!f.is_open()) continue;

                    std::ostringstream ss;
                    ss << f.rdbuf();
                    std::string content = ss.str();

                    // Check for template="true" in graph attributes
                    if (content.find("template=\"true\"") == std::string::npos &&
                        content.find("template=true") == std::string::npos) {
                        continue;
                    }

                    // Extract label
                    std::string label = name.substr(0, name.size() - 4);
                    auto label_pos = content.find("label=\"");
                    if (label_pos != std::string::npos) {
                        auto end_pos = content.find("\"", label_pos + 7);
                        if (end_pos != std::string::npos) {
                            label = content.substr(label_pos + 7, end_pos - label_pos - 7);
                        }
                    }

                    // Extract params: params="name:type:default, ..."
                    nlohmann::json params = nlohmann::json::array();
                    auto params_pos = content.find("params=\"");
                    if (params_pos != std::string::npos) {
                        auto params_end = content.find("\"", params_pos + 8);
                        if (params_end != std::string::npos) {
                            std::string params_str = content.substr(params_pos + 8, params_end - params_pos - 8);
                            // Parse comma-separated param definitions
                            std::istringstream pss(params_str);
                            std::string pdef;
                            while (std::getline(pss, pdef, ',')) {
                                // Trim
                                size_t s = pdef.find_first_not_of(" \t");
                                if (s == std::string::npos) continue;
                                pdef = pdef.substr(s);
                                // Parse name:type:default or name:type(options):default
                                nlohmann::json p;
                                std::istringstream parts(pdef);
                                std::string pname, ptype, pdefault;
                                std::getline(parts, pname, ':');
                                std::getline(parts, ptype, ':');
                                std::getline(parts, pdefault, ':');
                                p["name"] = pname;
                                // Parse type — could be "choice(a,b,c)"
                                auto paren = ptype.find('(');
                                if (paren != std::string::npos) {
                                    auto close = ptype.find(')', paren);
                                    std::string options_str = ptype.substr(paren + 1, close - paren - 1);
                                    p["type"] = ptype.substr(0, paren);
                                    nlohmann::json opts = nlohmann::json::array();
                                    std::istringstream oss(options_str);
                                    std::string opt;
                                    while (std::getline(oss, opt, '|')) {
                                        // Trim
                                        size_t os = opt.find_first_not_of(" ");
                                        if (os != std::string::npos) opt = opt.substr(os);
                                        opts.push_back(opt);
                                    }
                                    p["options"] = opts;
                                } else {
                                    p["type"] = ptype;
                                }
                                p["default"] = pdefault;
                                params.push_back(p);
                            }
                        }
                    }

                    nlohmann::json t;
                    t["name"] = name.substr(0, name.size() - 4);
                    t["label"] = label;
                    t["file"] = path;
                    t["params"] = params;
                    templates.push_back(t);
                }
            }

            res.set_content(templates.dump(), "application/json");
        });

        // ── /api/v1/templates/:name (GET) ───────────────────
        svr.Get(R"(/api/v1/templates/(\w[\w\-]*))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string name = req.matches[1];
            auto dir_result = config_.resource_locator.find_dir("sample_dots");
            std::string templates_dir = dir_result.ok() ? dir_result.value() : "sample_dots";
            std::string path = templates_dir + "/" + name + ".dot";

            std::ifstream f(path);
            if (!f.is_open()) {
                res.status = 404;
                res.set_content("{\"error\":\"template not found\"}", "application/json");
                return;
            }

            std::ostringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "text/plain; charset=utf-8");
        });

        // ── /api/v1/generate-dot (POST) ───────────────────────
        svr.Post("/api/v1/generate-dot", [this](const httplib::Request& req, httplib::Response& res) {
            nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid JSON\"}", "application/json");
                return;
            }

            std::string provider = body.value("provider", "anthropic");

            std::vector<ChatMessage> messages;
            if (body.contains("messages") && body["messages"].is_array()) {
                for (const auto& m : body["messages"]) {
                    ChatMessage cm;
                    cm.role = m.value("role", "user");
                    cm.content = m.value("content", "");
                    if (!cm.content.empty()) {
                        messages.push_back(std::move(cm));
                    }
                }
            }

            if (messages.empty()) {
                res.status = 400;
                res.set_content("{\"error\":\"no messages provided\"}", "application/json");
                return;
            }

            auto result = dot_generator_.generate(provider, messages);

            nlohmann::json j;
            if (result.ok()) {
                j["response"] = result.value();

                // Try to extract DOT source from ```dot ... ``` code block
                std::string text = result.value();
                std::string dot_source;
                std::string::size_type dot_start = text.find("```dot");
                if (dot_start == std::string::npos) {
                    dot_start = text.find("```graphviz");
                }
                if (dot_start != std::string::npos) {
                    auto nl = text.find('\n', dot_start);
                    if (nl != std::string::npos) {
                        auto dot_end = text.find("```", nl);
                        if (dot_end != std::string::npos) {
                            dot_source = text.substr(nl + 1, dot_end - nl - 1);
                        }
                    }
                }
                if (!dot_source.empty()) {
                    j["dot_source"] = dot_source;
                }
            } else {
                res.status = 502;
                j["error"] = result.error();
            }

            res.set_content(j.dump(), "application/json");
        });

        // ── /api/v1/config (GET) — Return redacted config ─────
        svr.Get("/api/v1/config", [](const httplib::Request& /*req*/, httplib::Response& res) {
            auto j = NeedleConfig::global().to_json_redacted();
            res.set_content(j.dump(2), "application/json");
        });

        // ── /api/v1/config (PUT) — Merge partial JSON into config ──
        svr.Put("/api/v1/config", [](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid JSON body\"}", "application/json");
                return;
            }
            auto r = NeedleConfig::global().merge(body);
            if (!r.ok()) {
                res.status = 500;
                nlohmann::json err;
                err["error"] = r.error();
                res.set_content(err.dump(), "application/json");
                return;
            }
            auto j = NeedleConfig::global().to_json_redacted();
            res.set_content(j.dump(2), "application/json");
        });

        // ── /api/v1/config/validate-key (POST) — Validate an API key ──
        svr.Post("/api/v1/config/validate-key", [](const httplib::Request& req, httplib::Response& res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.contains("provider")) {
                res.status = 400;
                res.set_content("{\"valid\":false,\"error\":\"missing provider\"}", "application/json");
                return;
            }
            std::string provider = body["provider"].get<std::string>();
            std::string api_key = NeedleConfig::global().resolve_api_key(provider);
            if (api_key.empty()) {
                nlohmann::json r;
                r["valid"] = false;
                r["error"] = "No API key configured for " + provider;
                res.set_content(r.dump(), "application/json");
                return;
            }

            // Make a lightweight API call to verify the key works
            CurlClient client;
            nlohmann::json result;

            if (provider == "openai") {
                auto resp = client.get("https://api.openai.com/v1/models",
                    {"Authorization: Bearer " + api_key}, 10000);
                result["valid"] = resp.ok() && resp.value().status_code == 200;
                if (!result["valid"]) result["error"] = "Invalid key or API error";

            } else if (provider == "anthropic") {
                // Send a minimal messages request; a 401 means bad key, anything else means it works
                std::string body_str = "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
                auto resp = client.post_json("https://api.anthropic.com/v1/messages",
                    {"x-api-key: " + api_key, "anthropic-version: 2023-06-01",
                     "Content-Type: application/json"}, body_str, 10000);
                if (resp.ok()) {
                    result["valid"] = resp.value().status_code != 401 && resp.value().status_code != 403;
                    if (!result["valid"]) result["error"] = "Invalid API key";
                } else {
                    result["valid"] = false;
                    result["error"] = resp.error();
                }

            } else if (provider == "gemini") {
                auto resp = client.get(
                    "https://generativelanguage.googleapis.com/v1beta/models",
                    {"x-goog-api-key: " + api_key}, 10000);
                result["valid"] = resp.ok() && resp.value().status_code == 200;
                if (!result["valid"]) result["error"] = "Invalid key or API error";

            } else if (provider == "tavily") {
                std::string body_str = "{\"api_key\":\"" + api_key + "\",\"query\":\"test\",\"max_results\":1}";
                auto resp = client.post_json("https://api.tavily.com/search",
                    {"Content-Type: application/json"}, body_str, 10000);
                if (resp.ok()) {
                    result["valid"] = resp.value().status_code == 200;
                    if (!result["valid"]) result["error"] = "Invalid API key";
                } else {
                    result["valid"] = false;
                    result["error"] = resp.error();
                }

            } else {
                result["valid"] = false;
                result["error"] = "Unknown provider: " + provider;
            }

            res.set_content(result.dump(), "application/json");
        });

        // ── /api/v1/models/:provider (GET) — Model list with caching ──
        svr.Get(R"(/api/v1/models/(\w+))", [](const httplib::Request& req, httplib::Response& res) {
            std::string provider = req.matches[1];

            // M6: Check cache first (thread-safe, no TOCTOU)
            auto cached = model_cache_instance().get_if_fresh(provider);
            if (cached.first) {
                nlohmann::json result;
                result["models"] = cached.second;
                result["cached"] = true;
                res.set_content(result.dump(), "application/json");
                return;
            }

            // Check if API key is configured
            std::string api_key = NeedleConfig::global().resolve_api_key(provider);

            nlohmann::json models;
            bool from_api = false;

#ifdef NEEDLE_HAS_CURL
            if (!api_key.empty() && provider != "anthropic") {
                models = fetch_models_from_api(provider, api_key);
                from_api = true;
            } else {
                models = fallback_models(provider);
            }
#else
            (void)api_key;
            models = fallback_models(provider);
#endif

            // M6: Store result in thread-safe cache
            model_cache_instance().store(provider, models);

            nlohmann::json result;
            result["models"] = models;
            result["cached"] = false;
            result["source"] = from_api ? "api" : "fallback";
            res.set_content(result.dump(), "application/json");
        });

        // ── /api/v1/events (GET) — Global SSE ─────────────────
        svr.Get("/api/v1/events", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");

            auto gq = global_queue_;
            size_t client_offset = 0;
            auto last_send = std::chrono::steady_clock::now();

            res.set_chunked_content_provider(
                "text/event-stream",
                [gq, client_offset, last_send]
                (size_t /*offset*/, httplib::DataSink& sink) mutable -> bool {
                    {
                        std::lock_guard<std::mutex> lock(gq->mutex);
                        while (client_offset < gq->events.size()) {
                            const std::string& evt = gq->events[client_offset];
                            sink.write(evt.c_str(), evt.size());
                            ++client_offset;
                            last_send = std::chrono::steady_clock::now();
                        }
                    }

                    // Heartbeat every 15 seconds
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_send).count();
                    if (elapsed >= 15) {
                        std::string hb = ":heartbeat\n\n";
                        sink.write(hb.c_str(), hb.size());
                        last_send = now;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    return true;  // keep connection open
                },
                [](bool /*success*/) {}
            );
        });

        // ── Legacy endpoints (unchanged) ───────────────────────

        // GET /pipelines - list all runs
        svr.Get("/pipelines", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            nlohmann::json j = nlohmann::json::array();
            std::lock_guard<std::mutex> lock(runs_mutex_);
            for (const auto& pair : runs_) {
                nlohmann::json run;
                run["id"] = pair.first;
                run["status"] = pair.second->get_status();
                j.push_back(std::move(run));
            }
            res.set_content(j.dump(), "application/json");
        });

        // POST /pipelines - start a new run
        svr.Post("/pipelines", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            auto run = create_run(graph_, "");

            nlohmann::json j;
            j["id"] = run->id;
            j["status"] = "running";
            res.status = 201;
            res.set_content(j.dump(), "application/json");
        });

        // GET /pipelines/:id - get run status
        svr.Get(R"(/pipelines/([\w-]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            std::lock_guard<std::mutex> lock(runs_mutex_);
            auto it = runs_.find(run_id);
            if (it == runs_.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"not found\"}", "application/json");
                return;
            }
            nlohmann::json j;
            j["id"] = it->first;
            j["status"] = it->second->get_status();
            j["event_count"] = it->second->collector.events().size();
            res.set_content(j.dump(), "application/json");
        });

        // POST /pipelines/:id/cancel - cancel a run
        svr.Post(R"(/pipelines/([\w-]+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];
            std::lock_guard<std::mutex> lock(runs_mutex_);
            auto it = runs_.find(run_id);
            if (it == runs_.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"not found\"}", "application/json");
                return;
            }
            it->second->cancelled.store(true);
            it->second->set_status("cancelled");
            nlohmann::json j;
            j["id"] = run_id;
            j["status"] = "cancelled";
            res.set_content(j.dump(), "application/json");
        });

        // GET /pipelines/:id/events - SSE stream (per-run)
        svr.Get(R"(/pipelines/([\w-]+)/events)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];

            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");

            size_t event_index = 0;

            res.set_chunked_content_provider(
                "text/event-stream",
                [run, event_index](size_t /*offset*/, httplib::DataSink& sink) mutable -> bool {
                    // Only copy events when there are new ones to send
                    if (run->collector.size() > event_index) {
                        auto current_events = run->collector.events();
                        while (event_index < current_events.size()) {
                            const auto& e = current_events[event_index];
                            std::string data = format_pipeline_sse(run->id, e);
                            sink.write(data.c_str(), data.size());
                            ++event_index;
                        }
                    }

                    if (run->get_status() != "running") {
                        sink.done();
                        return false;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    return true;
                },
                [](bool /*success*/) {}
            );
        });

        // POST /pipelines/:id/answer - submit human-in-the-loop answer
        svr.Post(R"(/pipelines/([\w-]+)/answer)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string run_id = req.matches[1];

            std::shared_ptr<PipelineRun> run;
            {
                std::lock_guard<std::mutex> lock(runs_mutex_);
                auto it = runs_.find(run_id);
                if (it == runs_.end()) {
                    res.status = 404;
                    res.set_content("{\"error\":\"not found\"}", "application/json");
                    return;
                }
                run = it->second;
            }

            auto http_iv = std::dynamic_pointer_cast<HttpInterviewer>(run->interviewer);
            if (http_iv) {
                http_iv->post_answer(req.body);
            }

            nlohmann::json j;
            j["status"] = "accepted";
            res.set_content(j.dump(), "application/json");
        });

        if (running_.load()) {
            if (!svr.bind_to_port(bind_addr_.c_str(), port_)) {
                NEEDLE_LOG_ERROR("server", "failed to bind HTTP server to %s:%d",
                                 bind_addr_.c_str(), port_);
                notify_error("failed to bind HTTP server to " + bind_addr_ +
                             ":" + std::to_string(port_));
                return;
            }

            notify_started("http://" + bind_addr_ + ":" + std::to_string(port_));
            svr.listen_after_bind();
        }
    });
}

void NeedleHttpServer::wait_until_ready() {
    if (svr_ptr_) {
        auto svr = std::static_pointer_cast<httplib::Server>(svr_ptr_);
        svr->wait_until_ready();
    }
}

void NeedleHttpServer::stop() {
    running_.store(false);

    // Resume any paused engines so they can see cancellation
    pause_controller_->resume();
    if (pause_timer_thread_.joinable()) {
        pause_timer_thread_.join();
    }

    // M9: Cancel all active runs so their engine loops exit.
    // Also wake any interactive sessions that are blocked on their CV — without
    // this, an interactive node waiting for user input holds the run_thread
    // indefinitely and join() hangs, forcing SIGKILL.
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        for (auto& kv : runs_) {
            kv.second->cancelled.store(true);
            if (kv.second->interactive_session) {
                std::lock_guard<std::mutex> sl(kv.second->interactive_session->mutex);
                kv.second->interactive_session->cv.notify_all();
            }
        }
    }

    // Stop the HTTP server first (so no new requests come in)
    if (stop_fn_) {
        stop_fn_();
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    notify_stopped();

    // M9: Now join all pipeline threads (they should exit quickly due to cancel)
    // Take a snapshot of runs to avoid holding runs_mutex_ during joins
    std::vector<std::shared_ptr<PipelineRun>> runs_snapshot;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        for (auto& kv : runs_) {
            runs_snapshot.push_back(kv.second);
        }
    }
    for (auto& run : runs_snapshot) {
        if (run->run_thread.joinable()) {
            NEEDLE_LOG_INFO("server", "joining pipeline thread for run %s", run->id.c_str());
            run->run_thread.join();
        }
    }

    // SPRINT-016 M11 fix: cancel and join any in-flight troubleshoot
    // worker threads so they can't access destroyed server state.
    {
        std::lock_guard<std::mutex> lock(troubleshoot_mutex_);
        for (auto& kv : troubleshoot_in_flight_) {
            if (kv.second.runner) kv.second.runner->kill_all();
            kv.second.active = false;
        }
    }
    std::vector<std::thread> threads_to_join;
    {
        std::lock_guard<std::mutex> lock(troubleshoot_threads_mutex_);
        threads_to_join.swap(troubleshoot_threads_);
    }
    for (auto& t : threads_to_join) {
        if (t.joinable()) t.join();
    }
}

// ── Observer hooks ────────────────────────────────────────────

void NeedleHttpServer::add_observer(std::shared_ptr<ServerObserver> observer) {
    if (observer) {
        observers_.push_back(std::move(observer));
    }
}

void NeedleHttpServer::notify_run_event(const std::string& run_id,
                                        const PipelineEvent& event) {
    for (auto& o : observers_) o->on_run_event(run_id, event);
}

void NeedleHttpServer::notify_idle() {
    for (auto& o : observers_) o->on_pipeline_idle();
}

void NeedleHttpServer::notify_started(const std::string& url) {
    for (auto& o : observers_) o->on_server_started(url);
}

void NeedleHttpServer::notify_stopped() {
    for (auto& o : observers_) o->on_server_stopped();
}

void NeedleHttpServer::notify_error(const std::string& message) {
    for (auto& o : observers_) o->on_error(message);
}

// ── Per-DOT subdirectory helpers ──────────────────────────────

std::string NeedleHttpServer::compute_logs_root(
    const std::string& project_dir, const std::string& dot_stem)
{
    return project_dir + "/.needle/" + dot_stem;
}

bool NeedleHttpServer::migrate_flat_needle_dir(
    const std::string& project_dir, const std::string& dot_stem)
{
    std::string old_root = project_dir + "/.needle";
    std::string new_root = old_root + "/" + dot_stem;

    // Only migrate if old-style flat checkpoint exists
    if (!platform::file_exists(old_root + "/checkpoint.json")) return false;

    // Already migrated?
    if (platform::file_exists(new_root + "/checkpoint.json")) return false;

    platform::mkdir_p(new_root);

    // Move checkpoint
    std::rename((old_root + "/checkpoint.json").c_str(),
                (new_root + "/checkpoint.json").c_str());

    // Clean up temp file if present
    if (platform::file_exists(old_root + "/checkpoint.json.tmp")) {
        platform::remove_file(old_root + "/checkpoint.json.tmp");
    }

    // Move stages directory contents
    if (platform::is_directory(old_root + "/stages")) {
        platform::mkdir_p(new_root + "/stages");
        auto entries = platform::list_directory(old_root + "/stages");
        for (const auto& entry : entries) {
            std::rename((old_root + "/stages/" + entry).c_str(),
                        (new_root + "/stages/" + entry).c_str());
        }
        platform::remove_dir(old_root + "/stages");
    }

    NEEDLE_LOG_INFO("migration", "migrated flat .needle/ to .needle/%s/", dot_stem.c_str());
    return true;
}

// ── RunRegistry ──────────────────────────────────────────────

// RunRegistry implementation now lives in src/util/run_registry.cpp

// ── Reconstruct run view from disk ───────────────────────────

nlohmann::json NeedleHttpServer::reconstruct_run_view_from_disk(const PipelineRun& run) const {
    nlohmann::json rv;
    rv["id"] = run.id;
    rv["status"] = run.get_status();
    std::string error = run.get_error();
    if (!error.empty()) rv["error"] = error;
    rv["dot_source"] = run.dot_source;
    rv["project_dir"] = run.project_dir;
    rv["dot_stem"] = run.dot_stem;
    rv["dry_run"] = run.dry_run;

    // Use ordered vectors to preserve execution order (std::map would sort alphabetically)
    std::vector<std::pair<std::string, std::string>> node_statuses_ordered;
    auto find_status = [&](const std::string& id) -> std::string* {
        for (auto& kv : node_statuses_ordered)
            if (kv.first == id) return &kv.second;
        return nullptr;
    };
    auto set_status = [&](const std::string& id, const std::string& st) {
        auto* existing = find_status(id);
        if (existing) *existing = st;
        else node_statuses_ordered.emplace_back(id, st);
    };

    size_t completed_stages = 0;
    std::string current_node;

    // Read checkpoint for completed_nodes (array preserves execution order)
    std::string cp_path = run.logs_root + "/checkpoint.json";
    if (platform::file_exists(cp_path)) {
        std::ifstream cf(cp_path);
        if (cf.is_open()) {
            auto cj = nlohmann::json::parse(cf, nullptr, false);
            if (!cj.is_discarded()) {
                if (cj.contains("completed_nodes") && cj["completed_nodes"].is_array()) {
                    for (const auto& n : cj["completed_nodes"]) {
                        if (n.is_string()) {
                            set_status(n.get<std::string>(), "completed");
                            ++completed_stages;
                        }
                    }
                }
                if (cj.contains("current_node") && cj["current_node"].is_string()) {
                    current_node = cj["current_node"].get<std::string>();
                }
            }
        }
    }

    // Scan stages directory for status.json files (appends any not already in checkpoint)
    std::string stages_dir = run.logs_root + "/stages";
    if (platform::is_directory(stages_dir)) {
        auto entries = platform::list_directory(stages_dir);
        for (const auto& entry : entries) {
            std::string status_path = stages_dir + "/" + entry + "/status.json";
            if (platform::file_exists(status_path)) {
                std::ifstream sf(status_path);
                if (sf.is_open()) {
                    auto sj = nlohmann::json::parse(sf, nullptr, false);
                    if (!sj.is_discarded() && sj.contains("status")) {
                        std::string st = sj["status"].get<std::string>();
                        if (st == "FAILURE") {
                            set_status(entry, "failed");
                        } else if (st == "SUCCESS" || st == "PARTIAL_SUCCESS") {
                            if (!find_status(entry)) {
                                set_status(entry, "completed");
                            }
                        }
                    }
                }
            }
        }
    }

    // Mark current_node as failed if run failed
    std::string run_status = run.get_status();
    if (!current_node.empty() && (run_status == "failed" || run_status == "cancelled")) {
        auto* st = find_status(current_node);
        if (!st || *st == "running") {
            set_status(current_node, "failed");
        }
    }

    // Compute total_stages from dot_source
    size_t total_stages = 0;
    if (!run.dot_source.empty()) {
        DotParser parser(run.dot_source);
        auto ast = parser.parse();
        if (ast.ok()) {
            GraphBuilder builder;
            auto gr = builder.build(ast.value());
            if (gr.ok()) {
                for (const auto& n : gr.value().nodes()) {
                    if (n.type != NodeType::START && n.type != NodeType::EXIT)
                        ++total_stages;
                }
            }
        }
    }

    rv["completed_stages"] = completed_stages;
    rv["total_stages"] = total_stages;
    rv["current_node"] = "";
    rv["pending_question"] = "";

    nlohmann::ordered_json ns = nlohmann::ordered_json::object();
    for (const auto& kv : node_statuses_ordered) ns[kv.first] = kv.second;
    rv["node_statuses"] = ns;
    rv["node_errors"] = nlohmann::json::object();
    rv["warnings"] = nlohmann::json::array();

    // Compute elapsed from manifest start_time/end_time, or from created_at
    double elapsed = 0.0;
    std::string manifest_path = run.logs_root + "/manifest.json";
    if (platform::file_exists(manifest_path)) {
        std::ifstream mf(manifest_path);
        if (mf.is_open()) {
            auto mj = nlohmann::json::parse(mf, nullptr, false);
            if (!mj.is_discarded()) {
                std::string st = mj.value("start_time", "");
                std::string et = mj.value("end_time", "");
                if (!st.empty() && !et.empty()) {
                    std::tm tm_s = {}, tm_e = {};
                    if (strptime(st.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_s) &&
                        strptime(et.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_e)) {
                        elapsed = difftime(timegm(&tm_e), timegm(&tm_s));
                        if (elapsed < 0) elapsed = 0;
                    }
                }
            }
        }
    }
    // Fallback: estimate from checkpoint timestamp vs created_at
    if (elapsed == 0.0 && !run.created_at.empty()) {
        std::string cp_ts;
        std::string cp_path2 = run.logs_root + "/checkpoint.json";
        if (platform::file_exists(cp_path2)) {
            std::ifstream cf2(cp_path2);
            if (cf2.is_open()) {
                auto cj2 = nlohmann::json::parse(cf2, nullptr, false);
                if (!cj2.is_discarded() && cj2.contains("timestamp")) {
                    cp_ts = cj2["timestamp"].get<std::string>();
                }
            }
        }
        if (!cp_ts.empty()) {
            std::tm tm_c = {}, tm_cp = {};
            if (strptime(run.created_at.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_c) &&
                strptime(cp_ts.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_cp)) {
                elapsed = difftime(timegm(&tm_cp), timegm(&tm_c));
                if (elapsed < 0) elapsed = 0;
            }
        }
    }
    rv["elapsed_seconds"] = elapsed;
    rv["event_count"] = 0;
    auto troubleshoot = troubleshoot_view_from_logs_root(run.logs_root);
    if (!troubleshoot.is_null()) rv["troubleshoot"] = troubleshoot;

    return rv;
}

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
