#include <catch2/catch.hpp>
#include <fstream>
#include <sstream>

#include "needle/parser/dot_parser.h"
#include "needle/parser/graph_builder.h"
#include "needle/validation/graph_validator.h"
#include "needle/engine/pipeline_engine.h"
#include "needle/event/event_bus.h"
#include "needle/event/collector_event_bus.h"
#include "needle/handlers/handler.h"
#include "needle/handlers/handler_registry.h"
#include "needle/platform/platform.h"

using namespace needle;

namespace {

class MockHandler : public Handler {
public:
    explicit MockHandler(const std::string& type, StageStatus status = StageStatus::SUCCESS)
        : type_(type), status_(status), call_count_(0) {}

    std::string type_name() const override { return type_; }

    Result<Outcome> execute(const Node& /*node*/, Context& /*ctx*/,
                            const ExecutionContext& /*exec_ctx*/) override {
        ++call_count_;
        Outcome o;
        o.status = status_;
        o.output = "mock output from " + type_;
        return Result<Outcome>::success(std::move(o));
    }

    int call_count() const { return call_count_; }

private:
    std::string type_;
    StageStatus status_;
    int call_count_;
};

std::string read_sample_dot(const std::string& filename) {
    // Try relative paths from likely test run locations
    std::vector<std::string> paths = {
        "sample_dots/" + filename,
        "../sample_dots/" + filename,
        "../../sample_dots/" + filename,
    };
    // Also try from NEEDLE_SOURCE_DIR if set
    const char* src_dir = std::getenv("NEEDLE_SOURCE_DIR");
    if (src_dir) {
        paths.insert(paths.begin(), std::string(src_dir) + "/sample_dots/" + filename);
    }

    for (const auto& path : paths) {
        std::ifstream f(path);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return "";
}

Result<Graph> parse_dot(const std::string& source) {
    DotParser parser(source);
    auto ast = parser.parse();
    if (!ast.ok()) {
        return Result<Graph>::failure(ast.error());
    }
    GraphBuilder builder;
    return builder.build(ast.value());
}

std::shared_ptr<HandlerRegistry> make_mock_registry() {
    auto registry = std::make_shared<HandlerRegistry>();
    std::vector<std::string> types = {
        "start", "exit", "codergen", "llmkit", "conditional",
        "parallel", "fan_in", "wait_human", "tool", "manager_loop"
    };
    for (const auto& t : types) {
        registry->register_handler(t, std::make_shared<MockHandler>(t));
    }
    return registry;
}

} // anonymous namespace

TEST_CASE("Integration: parse simple_pipeline.dot and run end-to-end", "[integration]") {
    std::string source = read_sample_dot("simple_pipeline.dot");
    if (source.empty()) {
        // If we cannot find the sample file, use an inline DOT
        source =
            "digraph simple_pipeline {\n"
            "    start [shape=Mdiamond, label=\"Start\"];\n"
            "    implement [shape=box, label=\"Implement\"];\n"
            "    validate [shape=box, label=\"Validate\"];\n"
            "    exit [shape=Msquare, label=\"Done\"];\n"
            "    start -> implement;\n"
            "    implement -> validate;\n"
            "    validate -> exit;\n"
            "}\n";
    }

    auto graph_result = parse_dot(source);
    REQUIRE(graph_result.ok());
    Graph graph = std::move(graph_result.value());

    // Validate
    GraphValidator validator = GraphValidator::create_default();
    Diagnostics diags = validator.validate(graph);
    REQUIRE_FALSE(diags.has_errors());

    // Run with mock handlers
    auto registry = make_mock_registry();

    auto cp_writer = std::make_shared<InMemoryCheckpointWriter>();

    PipelineConfig config;
    config.handler_registry = registry;
    config.checkpoint_writer = cp_writer;
    config.logs_root = platform::temp_dir() + "/needle_integration_test";

    PipelineEngine engine(std::move(config));

    Context ctx;
    EventBus bus;
    CollectorEventBus collector;

    bus.subscribe([&collector](const PipelineEvent& e) {
        collector.record(e);
    });

    auto result = engine.run(graph, ctx, bus);
    REQUIRE(result.ok());

    // Verify events in correct order
    auto events = collector.events();
    REQUIRE(events.size() >= 3);
    REQUIRE(events.front().type == EventType::PIPELINE_STARTED);
    REQUIRE(events.back().type == EventType::PIPELINE_COMPLETED);

    // Verify stage events come in pairs (STARTED/COMPLETED)
    int stage_started = 0;
    int stage_completed = 0;
    for (const auto& e : events) {
        if (e.type == EventType::STAGE_STARTED) ++stage_started;
        if (e.type == EventType::STAGE_COMPLETED) ++stage_completed;
    }
    REQUIRE(stage_started >= 3);  // start, implement, validate (exit may or may not count)
    REQUIRE(stage_started == stage_completed);

    // Verify checkpoint was created
    auto cp_result = cp_writer->load(platform::temp_dir() + "/needle_integration_test/checkpoint.json");
    REQUIRE(cp_result.ok());
    REQUIRE_FALSE(cp_result.value().completed_nodes.empty());
}

TEST_CASE("Integration: validate sample DOT files", "[integration]") {
    // Test that all sample DOT files parse and validate
    std::vector<std::string> filenames = {
        "simple_pipeline.dot",
        "parallel_pipeline.dot",
        "human_gate.dot",
        "goal_gate.dot",
        "multi_backend.dot"
    };

    for (const auto& filename : filenames) {
        SECTION(filename) {
            std::string source = read_sample_dot(filename);
            if (source.empty()) {
                WARN("Could not find " + filename + ", skipping");
                continue;
            }

            auto graph_result = parse_dot(source);
            REQUIRE(graph_result.ok());

            GraphValidator validator = GraphValidator::create_default();
            Diagnostics diags = validator.validate(graph_result.value());
            REQUIRE_FALSE(diags.has_errors());
        }
    }
}
