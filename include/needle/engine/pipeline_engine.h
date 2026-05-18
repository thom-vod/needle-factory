#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include "needle/model/result.h"
#include "needle/model/graph.h"
#include "needle/model/outcome.h"
#include "needle/model/context.h"
#include "needle/model/fidelity.h"
#include "needle/event/event_bus.h"
#include "needle/validation/diagnostic.h"
#include "needle/engine/edge_selector.h"
#include "needle/engine/checkpoint_manager.h"
#include "needle/engine/retry_controller.h"
#include "needle/engine/execution_context.h"
#include "needle/engine/subgraph_executor.h"
#include "needle/engine/transform.h"
#include "needle/engine/auto_troubleshoot.h"
#include "needle/handlers/handler_registry.h"
#include "needle/handlers/interactive_session.h"
#include "needle/backend/backend.h"
#include "needle/util/resource_locator.h"
#include "needle/worktree/strategy.h"
#include "needle/troubleshoot/types.h"

namespace needle {

class ProcessRunner;
struct PauseController;  // defined in engine/pause_controller.h; nullptr when not serving

struct PipelineConfig {
    std::string logs_root;
    std::string graph_file;
    std::string stylesheet_file;
    std::string project_dir;  // working directory for agents (default: ".")
    std::shared_ptr<Backend> cli_backend;  // for handlers that need it at per-run re-registration
    std::shared_ptr<ProcessRunner> process_runner;  // for cancellation (kill child processes)
    std::shared_ptr<CheckpointWriter> checkpoint_writer;
    std::shared_ptr<HandlerRegistry> handler_registry;
    std::shared_ptr<InteractiveSession> interactive_session;
    std::shared_ptr<EdgeSelector> edge_selector;
    std::vector<std::shared_ptr<Transform>> transforms;
    std::shared_ptr<PauseController> pause_controller;  // global pause state (nullptr = no pause support)
    ResourceLocator resource_locator;
    FidelityMode default_fidelity;
    bool auto_status;
    bool strict_graph_hash;  // resume blocks on hash mismatch for completed nodes (N3)
    bool allow_unresolved_vars;
    bool auto_troubleshoot;
    TroubleshootMode troubleshoot_mode;
    int max_attempts_per_stage;
    AutoTroubleshoot::RegisterRunnerFn troubleshoot_register_runner;
    WorktreeConfig worktree;
    // SPRINT-013 §3.4: content-level hash of the DOT source as the
    // caller (CLI router / HTTP server) loaded it, persisted on every
    // checkpoint save so resume can detect "DOT edited on disk".
    std::string dot_content_hash;

    PipelineConfig()
        : default_fidelity(FidelityMode::COMPACT)
        , auto_status(true)
        , strict_graph_hash(false)
        , allow_unresolved_vars(false)
        , auto_troubleshoot(false)
        , troubleshoot_mode(TroubleshootMode::Off)
        , max_attempts_per_stage(1) {}
};

// Session captures all mode-specific setup, then feeds into execute_loop() (M8)
struct ExecutionSession {
    Graph graph = Graph::make("", {}, {});
    Context* ctx = nullptr;         // owned by caller (run passes ref, resume makes a local)
    const Node* current = nullptr;
    const Node* exit_node = nullptr;
    EventBus* event_bus = nullptr;
    bool resume_mode = false;
};

class PipelineEngine : public SubgraphExecutor {
public:
    explicit PipelineEngine(PipelineConfig config);
    PipelineEngine(PipelineConfig config, std::atomic<bool>& external_cancelled);

    Result<void> run(const Graph& graph, Context& ctx, EventBus& event_bus);
    Result<void> resume(const Checkpoint& cp, const Graph& graph, EventBus& event_bus);

    // Cancel the pipeline and kill running processes
    void cancel();

    // SubgraphExecutor interface
    Result<Outcome> execute_subgraph(
        const std::string& start_node_id,
        const std::string& end_node_id,
        Context& ctx,
        const ExecutionContext& exec_ctx
    ) override;

    // M7: Per-branch retry controller for parallel execution
    Result<Outcome> execute_subgraph(
        const std::string& start_node_id,
        const std::string& end_node_id,
        Context& ctx,
        const ExecutionContext& exec_ctx,
        RetryController* branch_retry
    ) override;

    // Overload that controls whether the end node is executed. Used by the
    // parallel handler to stop branches before the fan-in node.
    Result<Outcome> execute_subgraph(
        const std::string& start_node_id,
        const std::string& end_node_id,
        Context& ctx,
        const ExecutionContext& exec_ctx,
        RetryController* branch_retry,
        bool inclusive_end
    ) override;

    // Record node completion from subgraph execution (M16: thread-safe)
    void record_node_completion(const Node& node, StageStatus status);

private:
    Result<void> execute_loop(ExecutionSession& session);
    void apply_common_node_context(const Node& current, Context& ctx, const ExecutionContext& exec_ctx);

    Result<Outcome> execute_node(const Node& node, Context& ctx, const ExecutionContext& exec_ctx);
    Result<const Edge*> select_next_edge(const Node& node, const Outcome& outcome,
                                         const Context& ctx, const ExecutionContext& exec_ctx);
    void save_checkpoint(const std::string& current_node, const Context& ctx);
    void check_goal_gates(const Graph& graph, const Context& ctx, Diagnostics& diags);
    void write_stage_directory(const Node& node, const Outcome& outcome);
    void write_manifest(const Graph& graph, const Context& ctx);
    void init_subgraph_executors();
    std::vector<std::pair<std::string, std::string>> apply_transforms(Graph& graph, Context& ctx, EventBus& event_bus);

    PipelineConfig config_;
    RetryController retry_controller_;
    std::mutex execution_state_mutex_;                  // guards completed_nodes_ and node_outcomes_ (M16)
    std::vector<std::string> completed_nodes_;
    std::map<std::string, StageStatus> node_outcomes_;  // per-node outcome for goal gate checks
    std::map<std::string, std::string> completed_node_hashes_;  // soft-hash check (N3)
    std::map<std::string, int> failure_signatures_;     // cycle detection: signature -> count
    int cycle_limit_;                                   // max identical failures before abort (default 3)
    std::atomic<bool> own_cancelled_;        // used when no external ref provided
    std::atomic<bool>& cancelled_;           // reference to own_ or external
    std::string start_time_;
    std::string current_graph_hash_;          // precomputed hash for checkpoint (M5 fix: no dangling pointer)
};

} // namespace needle
