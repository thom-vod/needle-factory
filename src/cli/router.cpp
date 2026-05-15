#include "router.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "needle/platform/platform.h"
#include "needle/util/run_registry.h"

#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/parser/stylesheet_parser.h"
#include "needle/validation/graph_validator.h"
#include "needle/validation/dot_linter.h"
#include "needle/engine/pipeline_engine.h"
#include "needle/engine/transform.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/engine/stage_advancer.h"
#include "needle/engine/edge_selector.h"
#include "needle/troubleshoot/diagnose.h"
#include "needle/event/event_bus.h"
#include "needle/event/collector_event_bus.h"
#include "needle/handlers/handler_registry.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/interactive_session.h"
#include "needle/backend/cli_backend.h"
#include "needle/backend/llmkit_backend.h"
#include "needle/backend/process_runner.h"
#include "needle/interviewer/interviewer.h"
#include "needle/model/fidelity.h"
#include "needle/model/context.h"
#include "needle/util/logger.h"
#include "needle/util/context_defaults.h"
#include "needle/config/needle_config.h"
#include "needle/util/uuid.h"
#include "needle/util/fs_helpers.h"
#include "needle/worktree/strategy.h"
#include "needle/rules/dot_authoring_rules.h"
#include "needle/rules/templates.h"

#ifdef NEEDLE_ENABLE_SERVER
#include "needle/server/http_server.h"
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

void make_directory(const std::string& path) {
    platform::make_dir(path);
}

// Extract model_stylesheet from graph attributes and parse it into a transform.
// Returns nullptr if not present or on parse error (with warning printed).
std::shared_ptr<Transform> parse_inline_stylesheet(const Graph& graph) {
    std::string ss_source = graph.graph_attrs().get("model_stylesheet");
    if (ss_source.empty()) return nullptr;
    auto ss_result = StylesheetParser::parse(ss_source);
    if (!ss_result.ok()) {
        std::cerr << "Warning: model_stylesheet: " << ss_result.error() << std::endl;
        return nullptr;
    }
    return make_stylesheet_transform(std::move(ss_result.value()));
}

// NoOp handler for --dry-run mode
class NoOpHandler : public Handler {
public:
    explicit NoOpHandler(const std::string& type) : type_(type) {}
    std::string type_name() const override { return type_; }
    Result<Outcome> execute(const Node& /*node*/, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        Outcome o;
        o.status = StageStatus::SUCCESS;
        o.output = "[dry-run] " + type_;
        return Result<Outcome>::success(std::move(o));
    }
private:
    std::string type_;
};

void console_event_callback(const PipelineEvent& event, bool no_color) {
    std::string prefix;
    std::string reset;
    if (!no_color) {
        switch (event.type) {
            case EventType::PIPELINE_STARTED:
            case EventType::PIPELINE_COMPLETED:
                prefix = "\033[1;32m"; // bold green
                break;
            case EventType::PIPELINE_FAILED:
            case EventType::STAGE_FAILED:
                prefix = "\033[1;31m"; // bold red
                break;
            case EventType::STAGE_RETRYING:
            case EventType::STAGE_WARNING:
            case EventType::VARIABLE_UNRESOLVED:
            case EventType::RESUME_WARNING:
                prefix = "\033[1;33m"; // bold yellow
                break;
            default:
                prefix = "\033[0;36m"; // cyan
                break;
        }
        reset = "\033[0m";
    }
    std::cout << prefix << "[" << event_type_to_string(event.type) << "]"
              << reset << " " << event.message;
    if (!event.node_id.empty()) {
        std::cout << " (node: " << event.node_id << ")";
    }
    std::cout << std::endl;
}

void json_event_callback(const PipelineEvent& event) {
    std::cout << event.to_json().dump() << std::endl;
}

/// Parse a duration string (e.g. "45m", "30s", "1800000") into milliseconds.
/// Returns 0 on failure.
int parse_duration_ms(const std::string& s) {
    if (s.empty()) return 0;
    try {
        size_t pos = 0;
        int val = std::stoi(s, &pos);
        if (pos == s.size()) return val;  // plain number = ms
        std::string suffix = s.substr(pos);
        if (suffix == "ms") return val;
        if (suffix == "s")  return val * 1000;
        if (suffix == "m")  return val * 60 * 1000;
        if (suffix == "h")  return val * 3600 * 1000;
        return 0;
    } catch (...) {
        return 0;
    }
}

/// Create a CLIBackend configured from NeedleConfig defaults.
/// Reads defaults.chat_agent, defaults.chat_model, and defaults.codergen_timeout
/// from the global config and applies them to all templates.
std::shared_ptr<Backend> create_cli_backend(std::shared_ptr<ProcessRunner> process_runner) {
    // Build all 3 provider templates
    std::map<std::string, CLITemplate> cli_templates;
    cli_templates["claude"] = CLITemplate::claude_default();
    cli_templates["codex"] = CLITemplate::codex_default();
    cli_templates["gemini"] = CLITemplate::gemini_default();

    // Read config overrides
    std::string cfg_agent = NeedleConfig::global().get_string("defaults.chat_agent");
    std::string cfg_model = NeedleConfig::global().get_string("defaults.chat_model");
    std::string cfg_timeout = NeedleConfig::global().get_string("defaults.codergen_timeout");

    // Apply model override to all templates if set
    if (!cfg_model.empty()) {
        for (auto& kv : cli_templates) {
            kv.second.defaults["model"] = cfg_model;
        }
    }

    // Apply timeout override to all templates if set
    int timeout_ms = parse_duration_ms(cfg_timeout);
    if (timeout_ms > 0) {
        for (auto& kv : cli_templates) {
            kv.second.default_timeout_ms = timeout_ms;
        }
    }

    // Select default template from config (fallback to claude)
    CLITemplate default_tmpl;
    auto it = cli_templates.find(cfg_agent);
    if (it != cli_templates.end()) {
        default_tmpl = it->second;
    } else {
        default_tmpl = cli_templates["claude"];
    }

    auto backend = std::make_shared<CLIBackend>(default_tmpl, cli_templates, process_runner);
    return backend;
}

void apply_worktree_config(PipelineConfig& config) {
    auto& nc = NeedleConfig::global();
    config.worktree.strategy =
        worktree_strategy_from_string(nc.get_string("worktree.strategy", "", "off"));
    config.worktree.branch = nc.get_string("worktree.branch_template", "", "auto/${run_id}");
    config.worktree.path = nc.get_string("worktree.path_template", "", "../${repo_basename}-wt-${run_id}");
    config.worktree.cleanup = nc.get_string("worktree.cleanup", "", "keep");
}

Result<Graph> parse_and_build(const std::string& dot_source) {
    DotParser parser(dot_source);
    auto ast_result = parser.parse();
    if (!ast_result.ok()) {
        return Result<Graph>::failure("Parse error: " + ast_result.error());
    }
    GraphBuilder builder;
    return builder.build(ast_result.value());
}

std::shared_ptr<HandlerRegistry> create_dry_run_registry() {
    auto registry = std::make_shared<HandlerRegistry>();
    std::vector<std::string> types = {
        "start", "exit", "codergen", "llmkit", "conditional",
        "parallel", "fan_in", "wait_human", "tool", "manager_loop",
        "web_search", "doc_fetch", "nested_run", "interactive"
    };
    for (const auto& t : types) {
        registry->register_handler(t, std::make_shared<NoOpHandler>(t));
    }
    return registry;
}

// Background thread that monitors an InteractiveSession and provides console I/O.
// Runs until `done` is set to true by the pipeline thread.
void console_interactive_loop(std::shared_ptr<InteractiveSession> session,
                              std::atomic<bool>& done) {
    std::ostream& out = std::cout;
    while (!done.load()) {
        // Wait for the session to become active
        {
            std::unique_lock<std::mutex> lock(session->mutex);
            session->cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return session->active || done.load();
            });
            if (done.load()) break;
            if (!session->active) continue;
        }

        // Print prompt and context
        out << "\n── Interactive: " << session->node_id << " ──" << std::endl;
        if (!session->prompt.empty()) {
            out << session->prompt << std::endl;
        }
        if (!session->context_summary.empty()) {
            out << "\nContext: " << session->context_summary << std::endl;
        }
        out << "\nEnter your response (empty line to finish):\n> " << std::flush;

        // Read lines until empty line or EOF
        std::string result;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) break;
            if (!result.empty()) result += "\n";
            result += line;
            out << "> " << std::flush;
        }

        // Signal the handler to continue
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->final_result = result;
            session->continued = true;
            session->cv.notify_one();
        }
    }
}

} // anonymous namespace

Router::Router(std::atomic<bool>& cancelled)
    : cancelled_(cancelled) {}

CLIArgs Router::parse_args(int argc, char* argv[]) {
    CLIArgs args;
    int i = 1;

    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.help = true;
            ++i;
        } else if (arg == "--version" || arg == "-v") {
            args.version = true;
            ++i;
        } else if (arg == "--logs-dir" && i + 1 < argc) {
            args.logs_dir = argv[++i];
            ++i;
        } else if (arg == "--stylesheet" && i + 1 < argc) {
            args.stylesheet = argv[++i];
            ++i;
        } else if (arg == "--backend" && i + 1 < argc) {
            args.backend = argv[++i];
            ++i;
        } else if (arg == "--interviewer" && i + 1 < argc) {
            args.interviewer_mode = argv[++i];
            ++i;
        } else if (arg == "--fidelity" && i + 1 < argc) {
            args.fidelity = argv[++i];
            ++i;
        } else if (arg == "--no-color") {
            args.no_color = true;
            ++i;
        } else if (arg == "--json") {
            args.json_output = true;
            ++i;
        } else if (arg == "--dry-run") {
            args.dry_run = true;
            ++i;
        } else if (arg == "--debug") {
            args.debug = true;
            ++i;
        } else if (arg == "--quiet") {
            args.quiet = true;
            ++i;
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = std::stoi(argv[++i]);
            ++i;
        } else if (arg == "--bind" && i + 1 < argc) {
            args.bind_addr = argv[++i];
            ++i;
        } else if (arg == "--var" && i + 1 < argc) {
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            if (eq != std::string::npos) {
                args.vars[kv.substr(0, eq)] = kv.substr(eq + 1);
            }
            ++i;
        } else if (arg == "--project-dir" && i + 1 < argc) {
            args.project_dir = argv[++i];
            ++i;
        } else if (arg == "--output" && i + 1 < argc) {
            args.stage_output = argv[++i];
            ++i;
        } else if (arg == "--to" && i + 1 < argc) {
            args.stage_to = argv[++i];
            ++i;
        } else if (arg == "--scope" && i + 1 < argc) {
            args.scope = argv[++i];
            ++i;
        } else if (arg == "--strict-graph-hash") {
            args.strict_graph_hash = true;
            ++i;
        } else if (arg == "--allow-unresolved-vars") {
            args.allow_unresolved_vars = true;
            ++i;
        } else if (arg == "--reload") {
            args.reload = true;
            ++i;
        } else if (arg == "--from-snapshot") {
            args.from_snapshot = true;
            ++i;
        } else if (arg == "--frozen-config") {
            args.frozen_config = true;
            ++i;
        } else if (arg == "--troubleshoot") {
            args.troubleshoot = true;
            ++i;
        } else if (arg == "--no-lint") {
            args.no_lint = true;
            ++i;
        } else if (arg == "--strict") {
            args.strict = true;
            ++i;
        } else if (arg[0] == '-') {
            std::cerr << "Unknown flag: " << arg << std::endl;
            ++i;
        } else {
            // Positional argument
            if (args.command.empty()) {
                args.command = arg;
            } else {
                args.positionals.push_back(arg);
            }
            ++i;
        }
    }

    return args;
}

int Router::dispatch(int argc, char* argv[]) {
    CLIArgs args = parse_args(argc, argv);

    // Configure global logger based on flags
    if (args.debug) {
        global_logger().set_level(LogLevel::TRACE);
    } else if (args.quiet) {
        global_logger().set_level(LogLevel::WARN);
    }

    // Set up central log file at ~/.needle/needle.log
    {
        const char* home = std::getenv("HOME");
        if (home) {
            std::string log_dir = std::string(home) + "/.needle";
            make_directory(log_dir);
            global_logger().set_file(log_dir + "/needle.log");
        }
    }

    // Load global config (merges file + defaults)
    NeedleConfig::global().load();

    // If no explicit --debug or --quiet, check config for logging level
    if (!args.debug && !args.quiet) {
        std::string cfg_level = NeedleConfig::global().get_string("logging.level");
        if (!cfg_level.empty()) {
            if (cfg_level == "trace") global_logger().set_level(LogLevel::TRACE);
            else if (cfg_level == "debug") global_logger().set_level(LogLevel::DEBUG);
            else if (cfg_level == "warn") global_logger().set_level(LogLevel::WARN);
            else if (cfg_level == "error") global_logger().set_level(LogLevel::ERROR);
            // "info" is already the default — no action needed
        }
    }

    if (args.version) {
        print_version();
        return 0;
    }

    if (args.help || args.command.empty()) {
        print_usage();
        return args.help ? 0 : 2;
    }

    if (args.frozen_config && args.command != "resume") {
        std::cerr << "Error: --frozen-config is only valid with resume" << std::endl;
        return 2;
    }

    if (args.command == "run") {
        return run_command(args);
    } else if (args.command == "resume") {
        return resume_command(args);
    } else if (args.command == "validate") {
        return validate_command(args);
    } else if (args.command == "dot-lint") {
        return dot_lint_command(args);
    } else if (args.command == "dot-rules") {
        return dot_rules_command(args);
    } else if (args.command == "template") {
        return template_command(args);
    } else if (args.command == "serve") {
        return serve_command(args);
    } else if (args.command == "auth") {
        return auth_command(args);
    } else if (args.command == "status") {
        return status_command(args);
    } else if (args.command == "config") {
        return config_command(args);
    } else if (args.command == "attach") {
        return attach_command(args);
    } else if (args.command == "retry") {
        return retry_command(args);
    } else if (args.command == "stage") {
        return stage_command(args);
    } else if (args.command == "troubleshoot") {
        return troubleshoot_command(args);
    } else {
        std::cerr << "Unknown command: " << args.command << std::endl;
        print_usage();
        return 2;
    }
}

int Router::run_command(const CLIArgs& args) {
    if (args.first_positional().empty()) {
        std::cerr << "Error: run requires a DOT file argument" << std::endl;
        return 2;
    }

    // Read DOT file
    std::string dot_source = read_file(args.first_positional());
    if (dot_source.empty()) {
        std::cerr << "Error: cannot read file: " << args.first_positional() << std::endl;
        return 1;
    }

    // Parse and build graph
    auto graph_result = parse_and_build(dot_source);
    if (!graph_result.ok()) {
        std::cerr << "Error: " << graph_result.error() << std::endl;
        return 1;
    }
    Graph graph = std::move(graph_result.value());

    // Parse and apply stylesheets (inline model_stylesheet first, then --stylesheet override)
    std::vector<std::shared_ptr<Transform>> transforms;
    if (auto t = parse_inline_stylesheet(graph)) {
        transforms.push_back(std::move(t));
    }
    if (!args.stylesheet.empty()) {
        std::string ss_source = read_file(args.stylesheet);
        if (ss_source.empty()) {
            std::cerr << "Error: cannot read stylesheet: " << args.stylesheet << std::endl;
            return 1;
        }
        auto ss_result = StylesheetParser::parse(ss_source);
        if (!ss_result.ok()) {
            std::cerr << "Error: stylesheet: " << ss_result.error() << std::endl;
            return 1;
        }
        transforms.push_back(make_stylesheet_transform(std::move(ss_result.value())));
    }
    transforms.push_back(make_variable_expansion_transform());

    // Validate
    GraphValidator validator = GraphValidator::create_default();
    Diagnostics diags = validator.validate(graph);
    diags.print(std::cerr, !args.no_color);

    if (diags.has_errors()) {
        std::cerr << "Validation failed with errors" << std::endl;
        return 1;
    }

    if (!args.no_lint) {
        DotLinter linter;
        auto warnings = linter.lint(graph, args.vars);
        for (const auto& w : warnings) {
            std::cerr << w.code << " " << w.message;
            if (!w.node_id.empty()) std::cerr << " (node: " << w.node_id << ")";
            std::cerr << std::endl;
        }
    }

    // Apply transforms
    Context ctx;

    // Pre-populate context with --var values
    for (const auto& kv : args.vars) {
        ctx.set("var." + kv.first, kv.second);
    }

    // Set project directory in context (defaults to cwd)
    std::string project_dir = args.project_dir;
    if (project_dir.empty()) {
        project_dir = platform::getcwd_str();
    }
    ctx.set("needle.project_dir", project_dir);

    // Set up logs directory — per-DOT subdirectory under project_dir/.needle/.
    // Computed BEFORE transforms so `{{logs_dir}}` placeholders in node
    // prompts/commands can be expanded via the context.
    std::string dot_stem = dot_stem_from_filename(args.first_positional());
    std::string logs_root;
    if (!args.logs_dir.empty()) {
        logs_root = args.logs_dir;
    } else if (!args.project_dir.empty()) {
        logs_root = args.project_dir + "/.needle/" + dot_stem;
    } else {
        logs_root = ".needle/" + dot_stem;
    }
    if (args.dry_run && args.logs_dir.empty()) {
        logs_root += "-dryrun";
    }
    ctx.set("needle.logs_root", logs_root);
    ctx.set("needle.logs_dir", logs_root + "/logs");
    ctx.set("needle.dot_stem", dot_stem);

    inject_config_defaults(ctx, NeedleConfig::global(), true);

    for (const auto& t : transforms) {
        auto t_result = t->apply(graph, ctx);
        if (!t_result.ok()) {
            std::cerr << "Error applying transform " << t->name() << ": " << t_result.error() << std::endl;
            return 1;
        }
    }

    // Resolve fidelity
    FidelityMode fidelity = FidelityMode::COMPACT;
    if (!args.fidelity.empty()) {
        fidelity = fidelity_from_string(args.fidelity);
    }
    // Migrate flat .needle/ to per-DOT subdirectory if needed
    {
        std::string needle_parent = !args.project_dir.empty()
            ? args.project_dir : platform::getcwd_str();
        std::string old_cp = needle_parent + "/.needle/checkpoint.json";
        std::string new_cp = needle_parent + "/.needle/" + dot_stem + "/checkpoint.json";
        if (platform::file_exists(old_cp) && !platform::file_exists(new_cp)) {
            platform::mkdir_p(needle_parent + "/.needle/" + dot_stem);
            std::rename(old_cp.c_str(), new_cp.c_str());
            std::string old_stages = needle_parent + "/.needle/stages";
            if (platform::is_directory(old_stages)) {
                std::string new_stages = needle_parent + "/.needle/" + dot_stem + "/stages";
                platform::mkdir_p(new_stages);
                auto entries = platform::list_directory(old_stages);
                for (const auto& e : entries) {
                    std::rename((old_stages + "/" + e).c_str(),
                                (new_stages + "/" + e).c_str());
                }
                platform::remove_dir(old_stages);
            }
            NEEDLE_LOG_INFO("migration", "migrated flat .needle/ to .needle/%s/", dot_stem.c_str());
        }
    }
    make_directory(logs_root);
    make_directory(logs_root + "/logs");

    // Create interactive session for CLI (enables interactive handler in console mode)
    auto interactive_session = std::make_shared<InteractiveSession>();

    // Create handler registry
    std::shared_ptr<HandlerRegistry> registry;
    if (args.dry_run) {
        registry = create_dry_run_registry();
    } else {
        // Create backends
        auto process_runner = std::make_shared<NativeProcessRunner>();
        std::shared_ptr<Backend> cli_backend = create_cli_backend(process_runner);

        std::map<std::string, ProviderConfig> providers;
        ProviderConfig anthropic;
        anthropic.name = "anthropic";
        anthropic.base_url = "https://api.anthropic.com/v1/messages";
        anthropic.api_key_env = "ANTHROPIC_API_KEY";
        anthropic.default_model = "claude-sonnet-4-20250514";
        providers["anthropic"] = anthropic;

        std::shared_ptr<Backend> llmkit_backend = std::make_shared<LLMKitBackend>(providers);

        // Create interviewer
        std::shared_ptr<Interviewer> interviewer_ptr;
        if (args.interviewer_mode == "auto") {
            interviewer_ptr = std::make_shared<AutoApproveInterviewer>();
        } else {
            interviewer_ptr = std::make_shared<ConsoleInterviewer>();
        }

        // We need a placeholder SubgraphExecutor - the engine itself implements it,
        // so we pass nullptr and the engine wires it up internally
        registry = HandlerRegistry::create_default(
            cli_backend, llmkit_backend, interviewer_ptr, nullptr, process_runner,
            interactive_session);
    }

    // Create checkpoint writer
    auto cp_writer = std::make_shared<JsonCheckpointWriter>();

    // Build pipeline config
    PipelineConfig config;
    apply_worktree_config(config);
    config.logs_root = logs_root;
    // Store graph_file as absolute path so resume works from any directory
    {
        std::string gf = args.first_positional();
        if (!gf.empty() && !platform::is_absolute_path(gf)) {
            gf = platform::path_join(platform::getcwd_str(), gf);
        }
        config.graph_file = gf;
    }
    config.stylesheet_file = args.stylesheet;
    config.project_dir = project_dir;
    config.checkpoint_writer = cp_writer;
    config.handler_registry = registry;
    config.edge_selector = std::make_shared<EdgeSelector>();
    config.transforms = transforms;
    config.default_fidelity = fidelity;
    config.allow_unresolved_vars = args.allow_unresolved_vars;
    config.dot_content_hash = std::to_string(std::hash<std::string>{}(dot_source));
    {
        bool graph_enabled = false;
        std::string v = graph.graph_attrs().get("troubleshoot_on_failure");
        if (v == "true" || v == "1") graph_enabled = true;
        config.auto_troubleshoot = args.troubleshoot || graph_enabled;
        int max_attempts = NeedleConfig::global().get_int("defaults.troubleshoot_max_attempts", 1);
        Maybe<int> graph_attempts = graph.graph_attrs().get_int("troubleshoot_max_attempts");
        if (graph_attempts.has_value() && *graph_attempts > 0) {
            max_attempts = *graph_attempts;
        }
        if (max_attempts < 1) max_attempts = 1;
        config.max_attempts_per_stage = max_attempts;
    }

    // Register run in the global registry so the dashboard can see it
    RunRegistry run_reg;
    run_reg.load();
    std::string run_id = RunRegistry::generate_run_id(project_dir);
    run_reg.add_entry(run_id, dot_stem, dot_source, project_dir,
                      logs_root, "running", utc_timestamp_now(), args.dry_run);
    run_reg.save();
    ctx.set("needle.run_id", run_id);

    // Create engine
    PipelineEngine engine(std::move(config));

    // Set up event bus
    EventBus event_bus;
    {
        bool no_color = args.no_color;
        bool json_out = args.json_output;
        event_bus.subscribe([no_color, json_out](const PipelineEvent& e) {
            if (json_out) {
                json_event_callback(e);
            } else {
                console_event_callback(e, no_color);
            }
        });
    }

    // Start console interactive thread (handles interactive handler nodes via stdin/stdout)
    std::atomic<bool> interactive_done(false);
    std::thread interactive_thread(console_interactive_loop,
                                   interactive_session,
                                   std::ref(interactive_done));

    // Run pipeline
    auto result = engine.run(graph, ctx, event_bus);

    // Stop interactive thread
    interactive_done.store(true);
    interactive_session->cv.notify_all();
    if (interactive_thread.joinable()) interactive_thread.join();

    // Update registry with final status
    run_reg.load(); // reload in case server modified it concurrently
    if (result.ok()) {
        run_reg.update_status(run_id, "completed");
    } else {
        run_reg.update_status(run_id, "failed", result.error());
        std::cerr << "Pipeline failed: " << result.error() << std::endl;
    }
    run_reg.save();

    if (!result.ok() && result.error().find("escalated auto-troubleshoot:") != std::string::npos) {
        return 3;
    }
    return result.ok() ? 0 : 1;
}

int Router::resume_command(const CLIArgs& args) {
    // Default to ./logs/checkpoint.json if no checkpoint file specified
    std::string checkpoint_path = args.first_positional();
    if (checkpoint_path.empty()) {
        checkpoint_path = ".needle/checkpoint.json";
    }

    bool json_out = args.json_output;

    // Load checkpoint
    JsonCheckpointWriter cp_writer;
    auto cp_result = cp_writer.load(checkpoint_path);
    if (!cp_result.ok()) {
        std::cerr << "Error loading checkpoint: " << cp_result.error() << std::endl;
        return 1;
    }
    const Checkpoint& cp = cp_result.value();

    // Resolve settings: CLI args override checkpoint values
    std::string stylesheet = args.stylesheet.empty() ? cp.stylesheet_file : args.stylesheet;
    std::string logs_root = args.logs_dir.empty() ? cp.logs_root : args.logs_dir;

    // Fallback: derive logs_root from checkpoint path
    if (logs_root.empty()) {
        size_t pos = checkpoint_path.rfind('/');
        if (pos != std::string::npos) {
            logs_root = checkpoint_path.substr(0, pos);
        }
    }

    // Re-parse the original graph
    std::string dot_source = read_file(cp.graph_file);
    if (dot_source.empty()) {
        std::cerr << "Error: cannot read original graph file: " << cp.graph_file << std::endl;
        return 1;
    }

    // SPRINT-013 §3.4: detect on-disk edits since run start. Without
    // --reload or --from-snapshot the resume aborts so the operator
    // makes an explicit choice instead of silently picking one.
    std::string current_hash = std::to_string(std::hash<std::string>{}(dot_source));
    if (!cp.dot_content_hash.empty() && cp.dot_content_hash != current_hash) {
        if (args.from_snapshot && !logs_root.empty()) {
            std::string snap_path = logs_root + "/source.dot";
            std::string snap = read_file(snap_path);
            if (!snap.empty()) {
                dot_source = snap;
                current_hash = std::to_string(std::hash<std::string>{}(dot_source));
            }
        }
        if (!args.reload && cp.dot_content_hash != current_hash) {
            std::cerr << "Error: DOT file on disk has changed since this run started.\n"
                      << "  snapshot hash: " << cp.dot_content_hash << "\n"
                      << "  current hash:  " << current_hash << "\n"
                      << "Use --reload to accept the new contents, or --from-snapshot\n"
                      << "to resume against the original frozen graph (if a snapshot\n"
                      << "is available at " << logs_root << "/source.dot)." << std::endl;
            return 1;
        }
    }

    auto graph_result = parse_and_build(dot_source);
    if (!graph_result.ok()) {
        std::cerr << "Error: " << graph_result.error() << std::endl;
        return 1;
    }
    Graph graph = std::move(graph_result.value());

    // Apply inline model_stylesheet from graph, then external stylesheet if available
    if (auto t = parse_inline_stylesheet(graph)) {
        Context tmp_ctx;
        t->apply(graph, tmp_ctx);
    }
    if (!stylesheet.empty()) {
        std::string nss_source = read_file(stylesheet);
        if (!nss_source.empty()) {
            auto ss_result = StylesheetParser::parse(nss_source);
            if (ss_result.ok()) {
                auto transform = make_stylesheet_transform(ss_result.value());
                Context tmp_ctx;
                transform->apply(graph, tmp_ctx);
            }
        }
    }

    GraphValidator validator = GraphValidator::create_default();
    Diagnostics diags = validator.validate(graph);
    diags.print(std::cerr, !args.no_color);
    if (diags.has_errors()) {
        std::cerr << "Validation failed with errors" << std::endl;
        return 1;
    }

    // Create handlers (same as run)
    auto process_runner = std::make_shared<NativeProcessRunner>();
    std::shared_ptr<Backend> cli_backend = create_cli_backend(process_runner);

    std::map<std::string, ProviderConfig> providers;
    ProviderConfig anthropic;
    anthropic.name = "anthropic";
    anthropic.base_url = "https://api.anthropic.com/v1/messages";
    anthropic.api_key_env = "ANTHROPIC_API_KEY";
    anthropic.default_model = "claude-sonnet-4-20250514";
    providers["anthropic"] = anthropic;
    std::shared_ptr<Backend> llmkit_backend = std::make_shared<LLMKitBackend>(providers);

    std::shared_ptr<Interviewer> interviewer_ptr;
    if (args.interviewer_mode == "auto") {
        interviewer_ptr = std::make_shared<AutoApproveInterviewer>();
    } else {
        interviewer_ptr = std::make_shared<ConsoleInterviewer>();
    }

    auto registry = HandlerRegistry::create_default(
        cli_backend, llmkit_backend, interviewer_ptr, nullptr, process_runner);

    PipelineConfig config;
    apply_worktree_config(config);
    config.logs_root = logs_root;
    config.graph_file = cp.graph_file;
    config.stylesheet_file = stylesheet;
    config.checkpoint_writer = std::make_shared<JsonCheckpointWriter>();
    config.handler_registry = registry;
    config.edge_selector = std::make_shared<EdgeSelector>();
    config.strict_graph_hash = args.strict_graph_hash;
    config.allow_unresolved_vars = args.allow_unresolved_vars;
    config.dot_content_hash = current_hash;
    {
        bool graph_enabled = false;
        std::string v = graph.graph_attrs().get("troubleshoot_on_failure");
        if (v == "true" || v == "1") graph_enabled = true;
        config.auto_troubleshoot = args.troubleshoot || graph_enabled;
        int max_attempts = NeedleConfig::global().get_int("defaults.troubleshoot_max_attempts", 1);
        Maybe<int> graph_attempts = graph.graph_attrs().get_int("troubleshoot_max_attempts");
        if (graph_attempts.has_value() && *graph_attempts > 0) {
            max_attempts = *graph_attempts;
        }
        if (max_attempts < 1) max_attempts = 1;
        config.max_attempts_per_stage = max_attempts;
    }

    PipelineEngine engine(std::move(config));

    EventBus event_bus;
    std::string resume_run_id = RunRegistry::generate_run_id(platform::getcwd_str());
    {
        bool no_color = args.no_color;
        event_bus.subscribe([no_color, json_out](const PipelineEvent& e) {
            if (json_out) {
                json_event_callback(e);
            } else {
                console_event_callback(e, no_color);
            }
        });
    }

    Checkpoint cp_mut = cp;
    cp_mut.context.set("needle.run_id", resume_run_id);
    if (!logs_root.empty()) {
        cp_mut.context.set("needle.logs_root", logs_root);
        cp_mut.context.set("needle.logs_dir", logs_root + "/logs");
        make_directory(logs_root + "/logs");
    }
    inject_config_defaults(cp_mut.context, NeedleConfig::global(), !args.frozen_config);
    auto result = engine.resume(cp_mut, graph, event_bus);
    if (!result.ok()) {
        std::cerr << "Pipeline resume failed: " << result.error() << std::endl;
        if (result.error().find("escalated auto-troubleshoot:") != std::string::npos) {
            return 3;
        }
        return 1;
    }

    return 0;
}

int Router::validate_command(const CLIArgs& args) {
    if (args.first_positional().empty()) {
        std::cerr << "Error: validate requires a DOT file argument" << std::endl;
        return 2;
    }

    std::string dot_source = read_file(args.first_positional());
    if (dot_source.empty()) {
        std::cerr << "Error: cannot read file: " << args.first_positional() << std::endl;
        return 1;
    }

    auto graph_result = parse_and_build(dot_source);
    if (!graph_result.ok()) {
        std::cerr << "Error: " << graph_result.error() << std::endl;
        return 1;
    }
    Graph graph = std::move(graph_result.value());

    // Apply inline model_stylesheet, then external stylesheet if provided
    if (auto t = parse_inline_stylesheet(graph)) {
        Context ctx;
        t->apply(graph, ctx);
    }
    if (!args.stylesheet.empty()) {
        std::string ss_source = read_file(args.stylesheet);
        if (ss_source.empty()) {
            std::cerr << "Error: cannot read stylesheet: " << args.stylesheet << std::endl;
            return 1;
        }
        auto ss_result = StylesheetParser::parse(ss_source);
        if (!ss_result.ok()) {
            std::cerr << "Error: stylesheet: " << ss_result.error() << std::endl;
            return 1;
        }
        Context ctx;
        auto transform = make_stylesheet_transform(std::move(ss_result.value()));
        auto t_res = transform->apply(graph, ctx);
        if (!t_res.ok()) {
            std::cerr << "Error applying stylesheet: " << t_res.error() << std::endl;
            return 1;
        }
    }

    GraphValidator validator = GraphValidator::create_default();
    Diagnostics diags = validator.validate(graph);

    if (args.json_output) {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& d : diags.all()) {
            nlohmann::json dj;
            dj["severity"] = (d.severity == DiagnosticSeverity::Error ? "error" :
                             d.severity == DiagnosticSeverity::Warning ? "warning" : "info");
            dj["code"] = d.code;
            dj["message"] = d.message;
            if (!d.node_id.empty()) {
                dj["node"] = d.node_id;
            }
            j.push_back(std::move(dj));
        }
        std::cout << j.dump(2) << std::endl;
    } else {
        diags.print(std::cout, !args.no_color);
        if (diags.all().empty()) {
            std::cout << "Validation passed: no issues found." << std::endl;
        } else if (!diags.has_errors()) {
            std::cout << "Validation passed with warnings." << std::endl;
        } else {
            std::cout << "Validation failed." << std::endl;
        }
    }

    return diags.has_errors() ? 1 : 0;
}

int Router::dot_lint_command(const CLIArgs& args) {
    if (args.first_positional().empty()) {
        std::cerr << "Error: dot-lint requires a DOT file argument" << std::endl;
        return 2;
    }
    std::string dot_source = read_file(args.first_positional());
    if (dot_source.empty()) {
        std::cerr << "Error: cannot read file: " << args.first_positional() << std::endl;
        return 1;
    }
    auto graph_result = parse_and_build(dot_source);
    if (!graph_result.ok()) {
        std::cerr << "Error: " << graph_result.error() << std::endl;
        return 1;
    }
    Graph graph = std::move(graph_result.value());
    DotLinter linter;
    auto warnings = linter.lint(graph, args.vars);
    if (args.json_output) {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& w : warnings) {
            j.push_back({
                {"code", w.code},
                {"node_id", w.node_id},
                {"message", w.message},
                {"line", w.line},
                {"severity", w.severity},
            });
        }
        std::cout << j.dump(2) << std::endl;
    } else {
        for (const auto& w : warnings) {
            std::cout << w.code << " " << w.message;
            if (!w.node_id.empty()) std::cout << " (node: " << w.node_id << ")";
            std::cout << std::endl;
        }
        std::cout << warnings.size() << " warnings." << std::endl;
    }
    if (args.strict && !warnings.empty()) return 1;
    return 0;
}

int Router::dot_rules_command(const CLIArgs& /*args*/) {
    std::cout << rules::kDotAuthoringRules << std::endl;
    return 0;
}

int Router::template_command(const CLIArgs& args) {
    if (args.positionals.empty()) {
        std::cerr << "usage: needle template list\n"
                  << "       needle template show <name>\n";
        return 2;
    }
    const std::string& sub = args.positionals[0];
    if (sub == "list") {
        for (const auto& name : templates::list_names()) {
            std::cout << name << "\n";
        }
        return 0;
    }
    if (sub == "show") {
        if (args.positionals.size() < 2) {
            std::cerr << "usage: needle template show <name>\n"
                      << "       (use `needle template list` for names)\n";
            return 2;
        }
        const char* content = templates::get(args.positionals[1]);
        if (!content) {
            std::cerr << "Error: no bundled template named '"
                      << args.positionals[1] << "'.\n"
                      << "Run `needle template list` to see available names." << std::endl;
            return 1;
        }
        std::cout << content;
        return 0;
    }
    std::cerr << "Error: unknown template subcommand '" << sub << "'.\n"
              << "usage: needle template list | needle template show <name>\n";
    return 2;
}

int Router::serve_command(const CLIArgs& args) {
#ifdef NEEDLE_ENABLE_SERVER
    bool json_out = args.json_output;

    Graph graph = Graph::make("needle", {}, {});

    if (!args.first_positional().empty()) {
        std::string dot_source = read_file(args.first_positional());
        if (dot_source.empty()) {
            std::cerr << "Error: cannot read file: " << args.first_positional() << std::endl;
            return 1;
        }

        auto graph_result = parse_and_build(dot_source);
        if (!graph_result.ok()) {
            std::cerr << "Error: " << graph_result.error() << std::endl;
            return 1;
        }
        graph = std::move(graph_result.value());

        GraphValidator validator = GraphValidator::create_default();
        Diagnostics diags = validator.validate(graph);
        if (diags.has_errors()) {
            diags.print(std::cerr, !args.no_color);
            return 1;
        }
        if (!args.no_lint) {
            DotLinter linter;
            auto warnings = linter.lint(graph, args.vars);
            for (const auto& w : warnings) {
                std::cerr << w.code << " " << w.message;
                if (!w.node_id.empty()) std::cerr << " (node: " << w.node_id << ")";
                std::cerr << std::endl;
            }
        }
    } else if (!json_out) {
        std::cout << "No DOT file specified. Starting with empty workspace." << std::endl;
        std::cout << "Open the dashboard to create a pipeline interactively." << std::endl;
    }

    // Apply inline model_stylesheet from graph, then external stylesheet if provided
    if (auto t = parse_inline_stylesheet(graph)) {
        Context tmp_ctx;
        t->apply(graph, tmp_ctx);
    }
    if (!args.stylesheet.empty()) {
        std::string ss_source = read_file(args.stylesheet);
        if (!ss_source.empty()) {
            auto ss_result = StylesheetParser::parse(ss_source);
            if (ss_result.ok()) {
                auto transform = make_stylesheet_transform(std::move(ss_result.value()));
                Context tmp_ctx;
                transform->apply(graph, tmp_ctx);
            }
        }
    }

    // Set up logs directory — under project_dir if set, else .needle in cwd
    std::string logs_root;
    if (!args.project_dir.empty() && args.logs_dir.empty()) {
        logs_root = args.project_dir + "/.needle";
    } else {
        logs_root = args.logs_dir.empty() ? ".needle" : args.logs_dir;
    }
    make_directory(logs_root);

    // Create real handler registry (not dry-run)
    auto process_runner = std::make_shared<NativeProcessRunner>();
    std::shared_ptr<Backend> cli_backend = create_cli_backend(process_runner);

    std::map<std::string, ProviderConfig> providers;
    ProviderConfig anthropic;
    anthropic.name = "anthropic";
    anthropic.base_url = "https://api.anthropic.com/v1/messages";
    anthropic.api_key_env = "ANTHROPIC_API_KEY";
    anthropic.default_model = "claude-sonnet-4-20250514";
    providers["anthropic"] = anthropic;
    std::shared_ptr<Backend> llmkit_backend = std::make_shared<LLMKitBackend>(providers);

    // Use queue interviewer for serve mode — human gates answered via HTTP API
    auto interviewer_ptr = std::make_shared<QueueInterviewer>(std::vector<InterviewAnswer>{});

    auto registry = HandlerRegistry::create_default(
        cli_backend, llmkit_backend, interviewer_ptr, nullptr, process_runner);

    // Resolve port and bind address: CLI flag > config file > built-in default
    int port = args.port > 0 ? args.port :
        NeedleConfig::global().get_int("server.port", 8080);
    std::string bind_addr = args.bind_addr.empty() ?
        NeedleConfig::global().get_string("server.bind", "", "127.0.0.1") : args.bind_addr;

    NeedleHttpServer server(port, bind_addr);

    PipelineConfig config;
    apply_worktree_config(config);
    config.graph_file = args.first_positional();
    config.stylesheet_file = args.stylesheet;
    config.logs_root = logs_root;
    config.cli_backend = cli_backend;
    config.process_runner = process_runner;
    config.handler_registry = registry;
    config.edge_selector = std::make_shared<EdgeSelector>();
    config.allow_unresolved_vars = args.allow_unresolved_vars;
    if (!logs_root.empty()) {
        config.checkpoint_writer = std::make_shared<JsonCheckpointWriter>();
    }

    EventBus event_bus;
    server.start(graph, std::move(config), event_bus);
    server.wait_until_ready();

    if (!json_out) {
        std::cout << "Listening on http://" << bind_addr << ":" << port << std::endl;
        std::cout << "Press Ctrl+C to stop." << std::endl;
    }

    // Wait for cancellation signal
    while (!cancelled_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    return 0;
#else
    (void)args;
    std::cerr << "Error: HTTP server not available. Rebuild with -DNEEDLE_BUILD_SERVER=ON" << std::endl;
    return 1;
#endif
}

int Router::status_command(const CLIArgs& args) {
    // Find checkpoint file
    std::string checkpoint_path = args.first_positional();
    if (checkpoint_path.empty()) {
        // Default: look under project_dir/.needle or .needle in cwd
        std::string base;
        if (!args.project_dir.empty()) {
            base = args.project_dir + "/.needle";
        } else {
            base = ".needle";
        }
        checkpoint_path = base + "/checkpoint.json";
    }

    // Try to load checkpoint
    std::string cp_str = read_file(checkpoint_path);
    if (cp_str.empty()) {
        std::cout << "No active run (no checkpoint at " << checkpoint_path << ")" << std::endl;
        return 0;
    }

    JsonCheckpointWriter cp_writer;
    auto cp_result = cp_writer.load(checkpoint_path);
    if (!cp_result.ok()) {
        std::cerr << "Error reading checkpoint: " << cp_result.error() << std::endl;
        return 1;
    }

    const Checkpoint& cp = cp_result.value();

    if (args.json_output) {
        std::cout << cp.to_json().dump(2) << std::endl;
        return 0;
    }

    // Human-readable output
    std::string prefix;
    std::string reset;
    if (!args.no_color) {
        prefix = "\033[1;36m"; // bold cyan
        reset = "\033[0m";
    }

    std::cout << prefix << "Pipeline Status" << reset << std::endl;
    std::cout << "  Graph:     " << cp.graph_file << std::endl;
    std::cout << "  Current:   " << (cp.current_node.empty() ? "(finished)" : cp.current_node) << std::endl;
    std::cout << "  Completed: " << cp.completed_nodes.size() << " nodes";
    if (!cp.completed_nodes.empty()) {
        std::cout << " [";
        for (size_t i = 0; i < cp.completed_nodes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << cp.completed_nodes[i];
        }
        std::cout << "]";
    }
    std::cout << std::endl;
    std::cout << "  Timestamp: " << cp.timestamp << std::endl;

    if (!cp.retry_counters.empty()) {
        std::cout << "  Retries:   ";
        bool first = true;
        for (const auto& kv : cp.retry_counters) {
            if (!first) std::cout << ", ";
            std::cout << kv.first << "=" << kv.second;
            first = false;
        }
        std::cout << std::endl;
    }

    return 0;
}

int Router::auth_command(const CLIArgs& args) {
    std::string provider = args.first_positional();
    if (provider.empty()) {
        std::cerr << "Error: auth requires a provider argument (chatgpt or gemini)" << std::endl;
        return 2;
    }

    if (provider != "chatgpt" && provider != "gemini") {
        std::cerr << "Error: unsupported provider '" << provider
                  << "' (use chatgpt or gemini)" << std::endl;
        return 2;
    }

    // Determine login URL
    std::string login_url;
    if (provider == "chatgpt") {
        login_url = "https://chatgpt.com";
    } else {
        login_url = "https://gemini.google.com";
    }

    // Check agent-browser is installed
    auto process_runner = std::make_shared<NativeProcessRunner>();
    if (!platform::command_exists("agent-browser")) {
        std::cerr << "Error: agent-browser not found. Install it from ../agent-browser" << std::endl;
        return 1;
    }

    // Ensure auth directory exists — we use --profile for a persistent Chrome
    // user-data directory, which preserves the full browser fingerprint and
    // avoids Cloudflare bot-detection loops that plague state save/load.
    std::string home = platform::home_dir();
    if (home.empty()) {
        std::cerr << "Error: home directory not found" << std::endl;
        return 1;
    }
    std::string auth_dir = platform::path_join(home, ".needle/auth");
    platform::mkdir_p(auth_dir);

    std::string profile_dir = auth_dir + "/" + provider;
    make_directory(profile_dir);

    // Launch system Chrome directly — NOT through agent-browser/Playwright.
    // Playwright patches navigator.webdriver and other JS APIs that Cloudflare
    // fingerprints, causing an infinite verification loop. By launching Chrome
    // directly with --user-data-dir, the user logs in as a normal human with
    // zero automation framework involvement. The resulting profile is then used
    // by agent-browser at runtime via --profile.
    std::string chrome_path = platform::find_chrome();
    if (chrome_path.empty()) {
        std::cerr << "Error: Google Chrome not found" << std::endl;
        std::cerr << "Install Chrome or set up auth manually." << std::endl;
        return 1;
    }

    std::cout << "Opening Chrome for " << provider << " login..." << std::endl;
    std::cout << "Navigating to " << login_url << std::endl;

    // Launch Chrome directly with a dedicated user-data-dir.
#ifdef __APPLE__
    auto open_result = process_runner->run(
        chrome_path, {
                 "--user-data-dir=" + profile_dir,
                 "--no-first-run",
                 "--no-default-browser-check",
                 login_url},
        ".", 15000);
#else
    auto open_result = process_runner->run(
        chrome_path, {
                 "--user-data-dir=" + profile_dir,
                 "--no-first-run",
                 "--no-default-browser-check",
                 login_url},
        ".", 15000);
#endif
    if (!open_result.ok() || open_result.value().exit_code != 0) {
        std::string err = open_result.ok() ? open_result.value().stderr_output : open_result.error();
        std::cerr << "Error: failed to open Chrome: " << err << std::endl;
        return 1;
    }

    std::cout << std::endl;
    std::cout << "==> Log in to " << provider << " in the Chrome window." << std::endl;
    std::cout << "==> When done, CLOSE Chrome completely, then press Enter here..." << std::endl;
    std::string dummy;
    std::getline(std::cin, dummy);

    // Verify the login actually worked by loading the profile with agent-browser,
    // navigating to the provider, and checking if we land on an authenticated page.
    std::cout << "Verifying login..." << std::endl;

    std::string verify_url;
    if (provider == "chatgpt") {
        verify_url = "https://chatgpt.com";
    } else {
        verify_url = "https://gemini.google.com/app";
    }

    // Navigate with the saved profile
    process_runner->run(
        "agent-browser",
        {"--profile", profile_dir,
         "--args", "--disable-blink-features=AutomationControlled",
         "open", verify_url},
        ".", 30000);

    // Wait for page to settle
    process_runner->run(
        "agent-browser",
        {"--profile", profile_dir, "wait", "--load", "networkidle"},
        ".", 15000);

    // Get the current URL — login pages redirect to different URLs than authenticated ones
    auto url_result = process_runner->run(
        "agent-browser",
        {"--profile", profile_dir, "get", "url"},
        ".", 10000);

    // Close the verification browser
    process_runner->run(
        "agent-browser", {"--profile", profile_dir, "close"}, ".", 10000);

    bool verified = false;
    if (url_result.ok() && url_result.value().exit_code == 0) {
        std::string final_url = url_result.value().stdout_output;
        // Trim whitespace
        while (!final_url.empty() &&
               (final_url.back() == '\n' || final_url.back() == '\r' || final_url.back() == ' ')) {
            final_url.pop_back();
        }

        if (provider == "chatgpt") {
            // Authenticated ChatGPT stays on chatgpt.com (not redirected to /auth/login)
            verified = final_url.find("/auth/login") == std::string::npos &&
                       final_url.find("login.") == std::string::npos &&
                       final_url.find("chatgpt.com") != std::string::npos;
        } else {
            // Authenticated Gemini stays on gemini.google.com/app
            verified = final_url.find("accounts.google.com") == std::string::npos &&
                       final_url.find("gemini.google.com") != std::string::npos;
        }

        if (!verified) {
            std::cerr << "Auth verification failed: landed on " << final_url << std::endl;
        }
    }

    if (!verified) {
        std::cerr << "Warning: could not verify " << provider << " login." << std::endl;
        std::cerr << "Profile saved at " << profile_dir << " but login may not have succeeded." << std::endl;
        std::cerr << "Run 'needle auth " << provider << "' again to retry." << std::endl;
        return 1;
    }

    std::cout << "Auth verified for " << provider << "." << std::endl;
    std::cout << "Profile saved at " << profile_dir << std::endl;
    return 0;
}

int Router::config_command(const CLIArgs& args) {
    std::string subcommand = args.first_positional();

    if (subcommand.empty() || subcommand == "list") {
        auto j = args.json_output ? NeedleConfig::global().to_json() : NeedleConfig::global().to_json_redacted();
        if (args.scope == "defaults") {
            if (j.contains("defaults")) {
                j = j["defaults"];
            } else {
                j = nlohmann::json::object();
            }
        }
        std::cout << j.dump(2) << std::endl;
        return 0;
    }

    if (subcommand == "path") {
        std::cout << NeedleConfig::config_path() << std::endl;
        return 0;
    }

    if (subcommand == "edit") {
        std::string editor;
        const char* env_editor = std::getenv("EDITOR");
        if (env_editor && env_editor[0] != '\0') {
            editor = env_editor;
        } else {
            editor = "vi";
        }
        std::string path = NeedleConfig::config_path();
        if (path.empty()) {
            std::cerr << "Error: cannot determine config path (HOME not set)" << std::endl;
            return 1;
        }
        // Ensure the config file exists (save defaults if needed)
        NeedleConfig::global().load();
        // Create the file if it does not exist yet
        {
            std::ifstream test(path);
            if (!test.is_open()) {
                auto sr = NeedleConfig::global().save();
                if (!sr.ok()) {
                    std::cerr << "Error: " << sr.error() << std::endl;
                    return 1;
                }
            }
        }
        std::string cmd = editor + " " + path;
        return ::system(cmd.c_str());
    }

    if (subcommand == "get") {
        if (args.positionals.size() < 2) {
            std::cerr << "Error: config get requires a key argument" << std::endl;
            std::cerr << "Usage: needle config get <key>" << std::endl;
            return 2;
        }
        std::string key = args.positionals[1];
        // Return raw value for scripting — check all JSON types
        auto j = NeedleConfig::global().to_json();
        // Walk the dot-notation path manually
        std::vector<std::string> parts;
        {
            std::string segment;
            for (char c : key) {
                if (c == '.') {
                    if (!segment.empty()) {
                        parts.push_back(segment);
                        segment.clear();
                    }
                } else {
                    segment += c;
                }
            }
            if (!segment.empty()) {
                parts.push_back(segment);
            }
        }
        const nlohmann::json* cur = &j;
        for (const auto& p : parts) {
            if (!cur->is_object() || cur->find(p) == cur->end()) {
                // Key not found — print nothing, exit 1
                return 1;
            }
            cur = &(*cur)[p];
        }
        if (cur->is_string()) {
            std::cout << cur->get<std::string>() << std::endl;
        } else {
            std::cout << cur->dump() << std::endl;
        }
        return 0;
    }

    if (subcommand == "set") {
        if (args.positionals.size() < 3) {
            std::cerr << "Error: config set requires a key and value" << std::endl;
            std::cerr << "Usage: needle config set <key> <value>" << std::endl;
            return 2;
        }
        std::string key = args.positionals[1];
        std::string value = args.positionals[2];
        auto r = NeedleConfig::global().set(key, value);
        if (!r.ok()) {
            std::cerr << "Error: " << r.error() << std::endl;
            return 1;
        }
        return 0;
    }

    if (subcommand == "unset") {
        if (args.positionals.size() < 2) {
            std::cerr << "Error: config unset requires a key argument" << std::endl;
            std::cerr << "Usage: needle config unset <key>" << std::endl;
            return 2;
        }
        std::string key = args.positionals[1];
        auto r = NeedleConfig::global().unset(key);
        if (!r.ok()) {
            std::cerr << "Error: " << r.error() << std::endl;
            return 1;
        }
        return 0;
    }

    std::cerr << "Unknown config subcommand: " << subcommand << std::endl;
    std::cerr << "Usage: needle config [list|get|set|unset|path|edit]" << std::endl;
    return 2;
}

void Router::print_usage() {
    std::cout <<
        "Usage: needle <command> [options]\n"
        "\n"
        "Commands:\n"
        "  run <graph.dot>           Run a pipeline\n"
        "  resume <checkpoint.json>  Resume from checkpoint\n"
        "  validate <graph.dot>      Validate a graph\n"
        "  dot-lint <graph.dot>      Lint a graph for semantic warnings\n"
        "  dot-rules                 Print canonical DOT authoring rules\n"
        "  template list             List bundled sample templates\n"
        "  template show <name>      Print a bundled template's DOT source\n"
        "  serve [graph.dot]         Start HTTP server (dot file optional)\n"
        "  status [checkpoint.json]  Show current run status\n"
        "  auth <provider>           Save browser auth (chatgpt or gemini)\n"
        "  attach <node_id>          Attach to a Claude session for a node (interactive debug)\n"
        "  config [subcommand]       Manage configuration\n"
        "    config list             Show all settings (keys redacted)\n"
        "    config get <key>        Get a single value (raw, for scripting)\n"
        "    config set <key> <val>  Set a value (dot-notation key)\n"
        "    config unset <key>      Remove a key\n"
        "    config path             Print config file path\n"
        "    config edit             Open config in $EDITOR\n"
        "\n"
        "Options:\n"
        "  --logs-dir DIR        Directory for run logs and checkpoints (default: .needle)\n"
        "  --var key=value       Set a context variable (repeatable)\n"
        "  --scope NAME          Scope for config list (e.g., defaults)\n"
        "  --stylesheet FILE     NSS stylesheet file\n"
        "  --backend cli|llmkit  Backend to use (default: cli)\n"
        "  --interviewer MODE    Interviewer: console, auto, queue\n"
        "  --fidelity MODE       Fidelity: full, summary_high, summary_medium, summary_low\n"
        "  --no-color            Disable colored output\n"
        "  --json                Output events as JSON lines on stdout\n"
        "  --dry-run             Use no-op handlers; default logs root is .needle/<stem>-dryrun\n"
        "  --debug               Enable debug/trace logging\n"
        "  --quiet               Suppress info-level log output\n"
        "  --port PORT           HTTP server port (default: 8080)\n"
        "  --bind ADDR           HTTP server bind address (default: 127.0.0.1)\n"
        "  --allow-unresolved-vars  Allow unresolved $var.* at run/resume start\n"
        "  --troubleshoot        Enable auto-troubleshoot on stage failure\n"
        "  --no-lint             Skip dot-lint warnings during run/serve\n"
        "  --strict              Strict mode for linting commands\n"
        "  --help, -h            Show this help\n"
        "  --version, -v         Show version\n";
}

int Router::attach_command(const CLIArgs& args) {
    // needle attach <node_id> [--project-dir <dir>]
    std::string node_id = args.first_positional();
    if (node_id.empty()) {
        std::cerr << "Usage: needle attach <node_id> [--project-dir <dir>]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Attach to a Claude session for a pipeline node." << std::endl;
        std::cerr << "Run from the project directory, or use --project-dir." << std::endl;
        return 2;
    }

    std::string project_dir = args.project_dir;
    if (project_dir.empty()) project_dir = ".";

    // Read session_id from stage directory (written by cli_backend before launch)
    std::string sid_path = project_dir + "/.needle/stages/" + node_id + "/session_id";
    std::ifstream sid_in(sid_path);
    std::string session_id;
    if (sid_in.is_open()) {
        std::getline(sid_in, session_id);
    }
    if (session_id.empty()) {
        std::cerr << "Error: no session_id found at " << sid_path << std::endl;
        std::cerr << "The node may not have been run yet." << std::endl;
        return 1;
    }

    std::cout << "Attaching to Claude session " << session_id << " for node '" << node_id << "'" << std::endl;
    std::cout << "Working directory: " << project_dir << std::endl;
    std::cout << "---" << std::endl;

    // Launch claude --resume interactively
    std::vector<std::string> argv_strs = {"claude", "--resume", session_id, "--model",
        NeedleConfig::global().get_string("defaults.chat_model", "", "claude-opus-4-7")};

    // Change to project directory
#ifdef _WIN32
    if (_chdir(project_dir.c_str()) != 0) {
#else
    if (chdir(project_dir.c_str()) != 0) {
#endif
        std::cerr << "Warning: could not chdir to " << project_dir << std::endl;
    }

    std::vector<char*> argv;
    for (auto& s : argv_strs) argv.push_back(&s[0]);
    argv.push_back(nullptr);

    execvp("claude", argv.data());
    // If we get here, exec failed
    std::cerr << "Error: failed to launch claude --resume " << session_id << std::endl;
    return 1;
}

int Router::retry_command(const CLIArgs& args) {
    if (args.first_positional().empty()) {
        std::cerr << "Error: retry requires a node ID argument" << std::endl;
        std::cerr << "Usage: needle retry <node_id> [--project-dir DIR]" << std::endl;
        return 2;
    }
    std::string target_node = args.first_positional();

    // Resolve project directory
    std::string project_dir = args.project_dir;
    if (project_dir.empty()) {
        project_dir = platform::getcwd_str();
    }

    // Find checkpoint — scan .needle/ subdirectories for one that contains this node
    std::string checkpoint_path;
    std::string needle_dir = project_dir + "/.needle";
    if (platform::is_directory(needle_dir)) {
        auto entries = platform::list_directory(needle_dir);
        for (const auto& entry : entries) {
            std::string candidate = needle_dir + "/" + entry + "/checkpoint.json";
            if (platform::file_exists(candidate)) {
                // Check if this checkpoint's graph contains the target node
                JsonCheckpointWriter tmp_writer;
                auto tmp_result = tmp_writer.load(candidate);
                if (tmp_result.ok()) {
                    const auto& cn = tmp_result.value().completed_nodes;
                    bool found = (tmp_result.value().current_node == target_node);
                    if (!found) {
                        for (const auto& id : cn) {
                            if (id == target_node) { found = true; break; }
                        }
                    }
                    if (!found) {
                        // Also check the graph itself
                        std::string dot = read_file(tmp_result.value().graph_file);
                        if (!dot.empty()) {
                            auto gr = parse_and_build(dot);
                            if (gr.ok() && gr.value().find_node(target_node)) {
                                found = true;
                            }
                        }
                    }
                    if (found) {
                        checkpoint_path = candidate;
                        break;
                    }
                }
            }
        }
        // Fallback to flat checkpoint
        if (checkpoint_path.empty()) {
            std::string flat = needle_dir + "/checkpoint.json";
            if (platform::file_exists(flat)) checkpoint_path = flat;
        }
    }

    if (checkpoint_path.empty()) {
        std::cerr << "Error: no checkpoint found containing node '" << target_node
                  << "' in " << needle_dir << std::endl;
        return 1;
    }

    // Load checkpoint
    JsonCheckpointWriter cp_writer;
    auto cp_result = cp_writer.load(checkpoint_path);
    if (!cp_result.ok()) {
        std::cerr << "Error loading checkpoint: " << cp_result.error() << std::endl;
        return 1;
    }
    Checkpoint cp = cp_result.value();

    // Re-parse graph
    std::string dot_source = read_file(cp.graph_file);
    if (dot_source.empty()) {
        std::cerr << "Error: cannot read graph file: " << cp.graph_file << std::endl;
        return 1;
    }
    auto graph_result = parse_and_build(dot_source);
    if (!graph_result.ok()) {
        std::cerr << "Error: " << graph_result.error() << std::endl;
        return 1;
    }
    Graph graph = std::move(graph_result.value());

    // Validate target node exists
    if (!graph.find_node(target_node)) {
        std::cerr << "Error: node '" << target_node << "' not found in graph" << std::endl;
        return 1;
    }

    // Compute nodes to remove: target and all reachable descendants
    auto to_remove = graph.reachable_from(target_node);

    // Remove from completed_nodes
    std::vector<std::string> new_completed;
    for (const auto& id : cp.completed_nodes) {
        if (!to_remove.count(id)) {
            new_completed.push_back(id);
        }
    }

    size_t removed = cp.completed_nodes.size() - new_completed.size();
    cp.completed_nodes = std::move(new_completed);
    cp.current_node = target_node;

    // Clear context keys produced by removed nodes
    auto all_ctx = cp.context.all();
    for (const auto& kv : all_ctx) {
        for (const auto& nid : to_remove) {
            if (kv.first.find("." + nid + ".") != std::string::npos ||
                kv.first.find("." + nid) == kv.first.size() - nid.size() - 1) {
                cp.context.set(kv.first, "");
            }
        }
    }

    // Save modified checkpoint
    cp_writer.save(cp, checkpoint_path);

    std::cerr << "Retry: reset to node '" << target_node << "' (removed "
              << removed << " completed nodes)" << std::endl;

    // Now resume from modified checkpoint — same as resume_command
    std::string stylesheet = args.stylesheet.empty() ? cp.stylesheet_file : args.stylesheet;
    std::string logs_root = cp.logs_root;
    if (logs_root.empty()) {
        size_t pos = checkpoint_path.rfind('/');
        if (pos != std::string::npos) logs_root = checkpoint_path.substr(0, pos);
    }

    // Apply stylesheets
    if (auto t = parse_inline_stylesheet(graph)) {
        Context tmp_ctx;
        t->apply(graph, tmp_ctx);
    }
    if (!stylesheet.empty()) {
        std::string nss_source = read_file(stylesheet);
        if (!nss_source.empty()) {
            auto ss_result = StylesheetParser::parse(nss_source);
            if (ss_result.ok()) {
                auto transform = make_stylesheet_transform(ss_result.value());
                Context tmp_ctx;
                transform->apply(graph, tmp_ctx);
            }
        }
    }

    // Create handlers
    auto process_runner = std::make_shared<NativeProcessRunner>();
    std::shared_ptr<Backend> cli_backend = create_cli_backend(process_runner);

    std::map<std::string, ProviderConfig> providers;
    ProviderConfig anthropic;
    anthropic.name = "anthropic";
    anthropic.base_url = "https://api.anthropic.com/v1/messages";
    anthropic.api_key_env = "ANTHROPIC_API_KEY";
    anthropic.default_model = "claude-sonnet-4-20250514";
    providers["anthropic"] = anthropic;
    std::shared_ptr<Backend> llmkit_backend = std::make_shared<LLMKitBackend>(providers);

    std::shared_ptr<Interviewer> interviewer_ptr;
    if (args.interviewer_mode == "auto") {
        interviewer_ptr = std::make_shared<AutoApproveInterviewer>();
    } else {
        interviewer_ptr = std::make_shared<ConsoleInterviewer>();
    }

    auto registry = HandlerRegistry::create_default(
        cli_backend, llmkit_backend, interviewer_ptr, nullptr, process_runner);

    PipelineConfig config;
    apply_worktree_config(config);
    config.logs_root = logs_root;
    config.graph_file = cp.graph_file;
    config.stylesheet_file = stylesheet;
    config.project_dir = project_dir;
    config.checkpoint_writer = std::make_shared<JsonCheckpointWriter>();
    config.handler_registry = registry;
    config.edge_selector = std::make_shared<EdgeSelector>();

    PipelineEngine engine(std::move(config));

    EventBus event_bus;
    bool no_color = args.no_color;
    bool json_out = args.json_output;
    event_bus.subscribe([no_color, json_out](const PipelineEvent& e) {
        if (json_out) {
            json_event_callback(e);
        } else {
            console_event_callback(e, no_color);
        }
    });

    // Register in run registry
    RunRegistry run_reg;
    run_reg.load();
    std::string run_id = RunRegistry::generate_run_id(project_dir);
    std::string dot_stem = dot_stem_from_filename(cp.graph_file);
    run_reg.add_entry(run_id, dot_stem, dot_source, project_dir,
                      logs_root, "running", utc_timestamp_now());
    run_reg.save();

    auto result = engine.resume(cp, graph, event_bus);

    run_reg.load();
    if (result.ok()) {
        run_reg.update_status(run_id, "completed");
    } else {
        run_reg.update_status(run_id, "failed", result.error());
        std::cerr << "Pipeline retry failed: " << result.error() << std::endl;
    }
    run_reg.save();

    return result.ok() ? 0 : 1;
}

void Router::print_version() {
    std::cout << "needle v0.1.0" << std::endl;
}

int Router::stage_command(const CLIArgs& args) {
    // `needle stage <subcommand> ...`
    //   stage mark <run-dir> <node-id> <success|failure> [--output "summary"]
    //   stage advance <run-dir> --to <node-id>
    if (args.positionals.empty()) {
        std::cerr << "Error: stage requires a subcommand (mark|advance)\n";
        std::cerr << "Usage:\n"
                  << "  needle stage mark <run-dir> <node-id> <success|failure> [--output \"summary\"]\n"
                  << "  needle stage advance <run-dir> --to <node-id>\n";
        return 2;
    }

    const std::string& sub = args.positionals[0];

    if (sub == "mark") {
        if (args.positionals.size() < 4) {
            std::cerr << "Error: stage mark requires <run-dir> <node-id> <success|failure>\n";
            return 2;
        }
        const std::string& run_dir = args.positionals[1];
        const std::string& node_id = args.positionals[2];
        const std::string& outcome = args.positionals[3];
        bool success;
        if (outcome == "success") {
            success = true;
        } else if (outcome == "failure") {
            success = false;
        } else {
            std::cerr << "Error: outcome must be 'success' or 'failure', got: " << outcome << "\n";
            return 2;
        }
        std::string output_text = args.stage_output;
        if (output_text.empty() && success) {
            output_text = "manual recovery: marked " + node_id + " as success";
        }
        auto result = StageAdvancer::mark(run_dir, node_id, success, output_text);
        if (!result.ok()) {
            std::cerr << "Error: " << result.error() << "\n";
            return 1;
        }
        std::cout << "Marked " << node_id << " as " << outcome << " in " << run_dir << "\n";
        return 0;
    }

    if (sub == "advance") {
        if (args.positionals.size() < 2) {
            std::cerr << "Error: stage advance requires <run-dir>\n";
            return 2;
        }
        const std::string& run_dir = args.positionals[1];
        if (args.stage_to.empty()) {
            std::cerr << "Error: stage advance requires --to <node-id> "
                         "(graph-aware next-node lookup is not yet implemented)\n";
            return 2;
        }
        auto result = StageAdvancer::advance(run_dir, args.stage_to);
        if (!result.ok()) {
            std::cerr << "Error: " << result.error() << "\n";
            return 1;
        }
        std::cout << "Advanced current_node to " << args.stage_to << " in " << run_dir << "\n";
        return 0;
    }

    std::cerr << "Error: unknown stage subcommand: " << sub << "\n";
    return 2;
}

int Router::troubleshoot_command(const CLIArgs& args) {
    if (args.first_positional().empty()) {
        std::cerr << "Error: troubleshoot requires a run-dir argument\n";
        std::cerr << "Usage: needle troubleshoot <run-dir>\n";
        std::cerr << "       (v1 ships diagnose-only; salvage and advance "
                     "follow in v2/v3)\n";
        return 2;
    }
    std::string run_dir = args.first_positional();

    DiagnosisReport report = Diagnose::collect_report(run_dir);
    if (report.signals.failed_node.empty()) {
        std::cerr << "Error: cannot identify failed stage in " << run_dir
                  << " (checkpoint missing or has no current_node)\n";
        return 1;
    }

    std::string md = Diagnose::render_markdown(report);

    // Write to <run-dir>/recovery-<timestamp>.md and stdout.
    std::string timestamp = utc_timestamp_now();
    // Replace ':' with '-' to make a filesystem-safe filename.
    for (auto& c : timestamp) {
        if (c == ':') c = '-';
    }
    std::string out_path = run_dir + "/recovery-" + timestamp + ".md";
    {
        std::ofstream out(out_path);
        if (out.is_open()) {
            out << md;
        }
    }
    std::cout << md;
    std::cerr << "\n[recovery report written to " << out_path << "]\n";

    return 0;
}

} // namespace needle
