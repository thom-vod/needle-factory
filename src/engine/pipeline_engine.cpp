#include "needle/engine/pipeline_engine.h"
#include "needle/engine/pause_controller.h"
#include "needle/engine/resume_validator.h"
#include "needle/engine/variable_expansion_transform.h"
#include "needle/engine/auto_troubleshoot.h"
#include "needle/handlers/all_handlers.h"
#include "needle/backend/process_runner.h"
#include "needle/event/event.h"
#include "needle/util/fs_helpers.h"
#include "needle/util/logger.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "needle/platform/platform.h"
#include "needle/config/needle_config.h"

namespace needle {

namespace {

void emit_event(EventBus& bus, EventType type, const std::string& node_id,
                const std::string& message, nlohmann::json data = nlohmann::json::object()) {
    PipelineEvent e;
    e.type = type;
    e.timestamp = utc_timestamp_now();
    e.node_id = node_id;
    e.message = message;
    e.data = std::move(data);
    bus.emit(e);
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string run_command_capture(const std::string& cmd) {
#ifdef _WIN32
    FILE* fp = _popen(cmd.c_str(), "r");
#else
    FILE* fp = popen(cmd.c_str(), "r");
#endif
    if (!fp) return "";
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), fp)) out += buf;
#ifdef _WIN32
    _pclose(fp);
#else
    pclose(fp);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

int current_process_id() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

} // anonymous namespace

static void apply_context_size_config() {
    auto& cfg = NeedleConfig::global();
    int kb = cfg.get_int("defaults.max_context_value_kb", 100);
    if (kb > 0) {
        Context::set_max_value_size(static_cast<size_t>(kb) * 1024);
    } else {
        Context::set_max_value_size(0);  // unlimited
    }
}

PipelineEngine::PipelineEngine(PipelineConfig config)
    : config_(std::move(config))
    , cycle_limit_(3)
    , own_cancelled_(false)
    , cancelled_(own_cancelled_)
    , current_graph_hash_()
{
    apply_context_size_config();
    init_subgraph_executors();
}

PipelineEngine::PipelineEngine(PipelineConfig config, std::atomic<bool>& external_cancelled)
    : config_(std::move(config))
    , cycle_limit_(3)
    , own_cancelled_(false)
    , cancelled_(external_cancelled)
    , current_graph_hash_()
{
    apply_context_size_config();
    init_subgraph_executors();
}

void PipelineEngine::cancel() {
    cancelled_.store(true);
    // Kill any active child processes via the shared process runner
    if (config_.process_runner) {
        config_.process_runner->kill_all();
    }
}

void PipelineEngine::init_subgraph_executors() {
    // Wire this engine as the SubgraphExecutor for parallel and manager_loop handlers.
    // The registry is created with nullptr because the engine doesn't exist yet at that point.
    if (config_.handler_registry) {
        auto* parallel = config_.handler_registry->get("parallel");
        auto* manager_loop = config_.handler_registry->get("manager_loop");

        // Re-register with this engine as executor
        if (parallel) {
            config_.handler_registry->register_handler("parallel",
                make_parallel_handler(std::shared_ptr<SubgraphExecutor>(
                    this, [](SubgraphExecutor*){}), config_.worktree));  // non-owning shared_ptr
        }
        if (manager_loop) {
            config_.handler_registry->register_handler("manager_loop",
                make_manager_loop_handler(std::shared_ptr<SubgraphExecutor>(
                    this, [](SubgraphExecutor*){})));  // non-owning shared_ptr
        }
    }
}

std::vector<std::pair<std::string, std::string>> PipelineEngine::apply_transforms(
    Graph& graph, Context& ctx, EventBus& event_bus) {
    auto ve_transform = make_typed_variable_expansion_transform();
    ve_transform->apply(graph, ctx);

    auto unresolved = ve_transform->unresolved_vars();
    for (const auto& pair : unresolved) {
        nlohmann::json data;
        data["node_id"] = pair.first;
        data["variable"] = pair.second;
        emit_event(event_bus, EventType::VARIABLE_UNRESOLVED, pair.first,
                   "Unresolved variable $" + pair.second + " in node " + pair.first,
                   std::move(data));
    }
    return unresolved;
}

void PipelineEngine::apply_common_node_context(const Node& current, Context& ctx,
                                                const ExecutionContext& exec_ctx) {
    auto in_edges = exec_ctx.graph.incoming_edges(current.id);
    const Edge* fidelity_edge = nullptr;
    for (const auto* e : in_edges) {
        if (!e->fidelity().empty()) {
            fidelity_edge = e;
            break;
        }
    }
    FidelityMode fm = resolve_fidelity(fidelity_edge, current, exec_ctx.graph);
    ctx.set("needle.fidelity_mode", to_string(fm));
    std::string goal = exec_ctx.graph.graph_attrs().get("goal");
    if (!goal.empty()) {
        ctx.set("needle.goal", goal);
    }
}

void PipelineEngine::record_node_completion(const Node& node, StageStatus status) {
    std::lock_guard<std::mutex> lock(execution_state_mutex_);
    // Only add successful nodes to completed list — failed nodes should not
    // appear in the checkpoint as completed, or resume will skip them.
    if (status != StageStatus::FAILURE && status != StageStatus::RETRY) {
        bool found = false;
        for (const auto& id : completed_nodes_) {
            if (id == node.id) { found = true; break; }
        }
        if (!found) {
            completed_nodes_.push_back(node.id);
        }
        // N3: capture per-node hash at completion time so the soft-hash
        // resume check can tell "graph edited on unstarted nodes" (safe)
        // apart from "graph edited on a node that already ran" (suspicious).
        completed_node_hashes_[node.id] = ResumeValidator::compute_node_hash(node);
    }
    node_outcomes_[node.id] = status;
}

// ── run() — setup + execute_loop() ──────────────────────────────

Result<void> PipelineEngine::run(const Graph& graph, Context& ctx, EventBus& event_bus) {
    start_time_ = utc_timestamp_now();
    cancelled_.store(false);
    completed_nodes_.clear();
    node_outcomes_.clear();
    failure_signatures_.clear();
    if (!config_.graph_file.empty()) {
        ctx.set("needle.graph_path", config_.graph_file);
    }

    // Make a mutable copy of the graph for variable expansion
    Graph mutable_graph = graph;
    auto unresolved = apply_transforms(mutable_graph, ctx, event_bus);
    if (!config_.allow_unresolved_vars) {
        std::vector<std::string> unresolved_refs;
        for (const auto& pair : unresolved) {
            const std::string& v = pair.second;
            // $var.* is always early-bound; $context.config.* is early-bound
            // (populated by CLI router / HTTP server before the engine starts).
            bool is_early =
                (v.size() > 4 && v.substr(0, 4) == "var.") ||
                v.rfind("context.config.", 0) == 0;
            if (is_early) {
                unresolved_refs.push_back(pair.first + ":$" + v);
            }
        }
        if (!unresolved_refs.empty()) {
            std::ostringstream oss;
            oss << "Unresolved variable references at run start: ";
            for (size_t i = 0; i < unresolved_refs.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << unresolved_refs[i];
            }
            emit_event(event_bus, EventType::PIPELINE_FAILED, "", oss.str());
            return Result<void>::failure(oss.str());
        }
    }

    ExecutionSession session;
    session.graph = std::move(mutable_graph);
    session.ctx = &ctx;
    session.event_bus = &event_bus;
    session.resume_mode = false;
    current_graph_hash_ = ResumeValidator::compute_graph_hash(session.graph);

    if (!config_.logs_root.empty()) {
        platform::mkdir_p(config_.logs_root);
        std::ofstream pid_out(config_.logs_root + "/engine.pid");
        if (pid_out.is_open()) {
            pid_out << current_process_id() << "\n";
        }
    }

    NEEDLE_LOG_INFO("engine", "pipeline started");
    emit_event(event_bus, EventType::PIPELINE_STARTED, "", "Pipeline started");

    // Find start and exit nodes
    const Node* start = session.graph.start_node();
    if (!start) {
        emit_event(event_bus, EventType::PIPELINE_FAILED, "", "No start node found");
        return Result<void>::failure("no start node found");
    }

    const Node* exit_node = session.graph.exit_node();
    if (!exit_node) {
        emit_event(event_bus, EventType::PIPELINE_FAILED, "", "No exit node found");
        return Result<void>::failure("no exit node found");
    }

    session.current = start;
    session.exit_node = exit_node;

    // Read cycle detection limit from graph attributes
    {
        Maybe<int> limit = session.graph.graph_attrs().get_int("loop_restart_signature_limit");
        if (limit.has_value() && *limit > 0) {
            cycle_limit_ = *limit;
        }
    }

    auto result = execute_loop(session);
    if (!config_.logs_root.empty()) {
        platform::remove_file(config_.logs_root + "/engine.pid");
    }
    return result;
}

// ── resume() — checkpoint restore + execute_loop() ──────────────

Result<void> PipelineEngine::resume(const Checkpoint& cp, const Graph& graph, EventBus& event_bus) {
    // Reconstruct state from checkpoint
    completed_nodes_ = cp.completed_nodes;
    completed_node_hashes_ = cp.completed_node_hashes;
    node_outcomes_.clear();
    // record_node_completion() only appends SUCCESS / PARTIAL_SUCCESS to
    // completed_nodes_, so treating every restored entry as SUCCESS is
    // safe. Without this, the goal-gate check after the exit node sees
    // every prior-segment goal-gate node as missing-from-node_outcomes_
    // and triggers a phantom retry-target cascade ("rogue run after
    // approval").
    for (const auto& id : completed_nodes_) {
        node_outcomes_[id] = StageStatus::SUCCESS;
    }
    failure_signatures_.clear();
    start_time_ = cp.timestamp;

    // Intentional policy: reset retry budgets on resume to give the pipeline a fresh chance.
    retry_controller_.reset();
    NEEDLE_LOG_WARN("engine", "Retry budgets reset for resumed pipeline — nodes will get fresh retry allowances");

    // Create context from checkpoint
    Context ctx = cp.context.clone();
    if (!config_.graph_file.empty()) {
        ctx.set("needle.graph_path", config_.graph_file);
    } else if (!cp.graph_file.empty()) {
        ctx.set("needle.graph_path", cp.graph_file);
    }

    Graph mutable_graph = graph;

    ExecutionSession session;
    session.ctx = &ctx;
    session.event_bus = &event_bus;
    session.resume_mode = true;

    auto unresolved = apply_transforms(mutable_graph, ctx, event_bus);
    if (!config_.allow_unresolved_vars) {
        for (const auto& pair : unresolved) {
            if (pair.second.size() > 4 && pair.second.substr(0, 4) == "var.") {
                std::string msg = "Unresolved $var reference at resume start: "
                                + pair.first + ":$" + pair.second;
                emit_event(event_bus, EventType::PIPELINE_FAILED, "", msg);
                return Result<void>::failure(msg);
            }
        }
    }
    session.graph = std::move(mutable_graph);
    current_graph_hash_ = ResumeValidator::compute_graph_hash(session.graph);

    // Validate checkpoint against graph before resuming. The graph attribute
    // `strict_hash_check=true` and the engine config flag both elevate
    // soft-hash warnings to errors.
    bool strict = config_.strict_graph_hash;
    if (!strict) {
        std::string g_strict = session.graph.graph_attrs().get("strict_hash_check");
        if (g_strict == "true" || g_strict == "1") strict = true;
    }
    Diagnostics resume_diags = ResumeValidator::validate(cp, session.graph, strict);
    for (const auto& d : resume_diags.all()) {
        if (d.severity == DiagnosticSeverity::Error) {
            emit_event(event_bus, EventType::RESUME_WARNING, "", d.message);
            return Result<void>::failure("Resume blocked: " + d.message);
        }
    }
    for (const auto& d : resume_diags.all()) {
        if (d.severity != DiagnosticSeverity::Error) {
            emit_event(event_bus, EventType::RESUME_WARNING, "", d.message);
        }
    }

    // Find the current node to resume from
    const Node* current = session.graph.find_node(cp.current_node);
    if (!current) {
        emit_event(event_bus, EventType::PIPELINE_FAILED, "",
                   "Resume node not found: " + cp.current_node);
        return Result<void>::failure("resume node not found: " + cp.current_node);
    }

    const Node* exit_node = session.graph.exit_node();
    if (!exit_node) {
        emit_event(event_bus, EventType::PIPELINE_FAILED, "", "No exit node found");
        return Result<void>::failure("no exit node found");
    }

    cancelled_.store(false);
    emit_event(event_bus, EventType::PIPELINE_STARTED, "", "Pipeline resumed from: " + cp.current_node);

    // Emit STAGE_COMPLETED for all previously completed nodes so the UI shows them as green
    for (const auto& node_id : cp.completed_nodes) {
        emit_event(event_bus, EventType::STAGE_COMPLETED, node_id,
                   "Previously completed: " + node_id);
    }

    // Read cycle detection limit from graph attributes
    {
        Maybe<int> limit = session.graph.graph_attrs().get_int("loop_restart_signature_limit");
        if (limit.has_value() && *limit > 0) {
            cycle_limit_ = *limit;
        }
    }

    // The checkpoint saves current_node as the last completed node.
    // Advance past it to the next node so we don't re-execute it.
    {
        ExecutionContext exec_ctx{session.graph, event_bus, config_.logs_root, config_.project_dir, config_.default_fidelity, cancelled_};

        bool already_completed = false;
        for (const auto& id : cp.completed_nodes) {
            if (id == current->id) { already_completed = true; break; }
        }
        if (already_completed) {
            // Select next edge from the completed node to find where to resume
            Outcome skip_outcome;
            skip_outcome.status = StageStatus::SUCCESS;
            // Restore preferred_label from context so human gate edges resolve correctly
            skip_outcome.preferred_label = ctx.get("human.gate.selected");
            auto edge_result = select_next_edge(*current, skip_outcome, ctx, exec_ctx);
            if (edge_result.ok()) {
                current = session.graph.find_node(edge_result.value()->to);
                if (current) {
                    emit_event(event_bus, EventType::STAGE_STARTED, "",
                               "Skipping already-completed node: " + cp.current_node +
                               ", advancing to: " + current->id);
                }
            }
        }
    }

    session.current = current;
    session.exit_node = exit_node;

    auto result = execute_loop(session);
    if (!config_.logs_root.empty()) {
        platform::remove_file(config_.logs_root + "/engine.pid");
    }
    return result;
}

// ── execute_loop() — unified main loop (M8) ─────────────────────

Result<void> PipelineEngine::execute_loop(ExecutionSession& session) {
    Context& ctx = *session.ctx;
    EventBus& event_bus = *session.event_bus;
    const Node* current = session.current;
    const Node* exit_node = session.exit_node;

    ExecutionContext exec_ctx{session.graph, event_bus, config_.logs_root, config_.project_dir, config_.default_fidelity, cancelled_};

    // Outer loop replaces goto labels: when a failure routing cascade or loop restart
    // needs to jump back to the beginning, we use `continue` on this outer loop.
    for (;;) {
        while (current && current->id != exit_node->id && !cancelled_.load()) {
            // Check global pause state — block until unpaused or cancelled
            if (config_.pause_controller && config_.pause_controller->is_paused()) {
                NEEDLE_LOG_INFO("engine", "paused before node: %s", current->id.c_str());
                emit_event(event_bus, EventType::STAGE_PAUSED, current->id,
                           "Node paused: " + current->id);
                if (!config_.pause_controller->wait_if_paused(cancelled_)) {
                    break;  // cancelled while paused
                }
                NEEDLE_LOG_INFO("engine", "resumed, starting node: %s", current->id.c_str());
            }

            NEEDLE_LOG_DEBUG("engine", "executing node: %s (type: %s)", current->id.c_str(), current->handler_type().c_str());
            emit_event(event_bus, EventType::STAGE_STARTED, current->id,
                       "Stage started: " + current->id);
            if (!config_.logs_root.empty()) {
                std::string stage_dir = config_.logs_root + "/stages/" + current->id;
                platform::mkdir_p(stage_dir);
                if (!config_.project_dir.empty()) {
                    std::string head = run_command_capture("git -C " + shell_quote(config_.project_dir) + " rev-parse HEAD 2>/dev/null");
                    if (!head.empty()) {
                        std::ofstream start_out(stage_dir + "/start_commit.txt");
                        if (start_out.is_open()) start_out << head << "\n";
                    }
                }
            }

            // Look up handler
            Handler* handler = nullptr;
            if (config_.handler_registry) {
                handler = config_.handler_registry->get(current->handler_type());
            }
            if (!handler) {
                std::string err = "no handler registered for type: " + current->handler_type();
                NEEDLE_LOG_ERROR("engine", "%s", err.c_str());
                emit_event(event_bus, EventType::STAGE_FAILED, current->id, err);
                emit_event(event_bus, EventType::PIPELINE_FAILED, "", err);
                return Result<void>::failure(err);
            }

            // Set fidelity mode and goal in context
            apply_common_node_context(*current, ctx, exec_ctx);

            // Execute handler
            Result<Outcome> outcome_result = handler->execute(*current, ctx, exec_ctx);
            if (!outcome_result.ok()) {
                NEEDLE_LOG_ERROR("engine", "node %s failed: %s", current->id.c_str(), outcome_result.error().c_str());
                emit_event(event_bus, EventType::STAGE_FAILED, current->id, outcome_result.error());
                emit_event(event_bus, EventType::PIPELINE_FAILED, "", outcome_result.error());
                return Result<void>::failure(outcome_result.error());
            }

            Outcome outcome = outcome_result.value();

            // Handle RETRY
            if (outcome.status == StageStatus::RETRY) {
                RetryPolicy policy = RetryPolicy::from_attributes(current->attrs);
                if (retry_controller_.should_retry(current->id, policy)) {
                    retry_controller_.record_attempt(current->id);
                    std::string retry_msg = outcome.output.empty()
                        ? ("Retrying stage: " + current->id)
                        : (outcome.output + " — retrying " + current->id);
                    emit_event(event_bus, EventType::STAGE_RETRYING, current->id, retry_msg);
                    if (outcome.retry_after_ms > 0) {
                        NEEDLE_LOG_INFO("engine", "node %s: waiting %dms (server-specified)",
                                        current->id.c_str(), outcome.retry_after_ms);
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(outcome.retry_after_ms));
                    } else {
                        retry_controller_.sleep_before_retry(current->id, policy);
                    }
                    continue; // Same node
                }
                // Exhausted retries -- check allow_partial
                Maybe<bool> allow_partial = current->attrs.get_bool("allow_partial");
                if (allow_partial.has_value() && *allow_partial) {
                    outcome.status = StageStatus::PARTIAL_SUCCESS;
                    outcome.output = "retries exhausted, partial accepted";
                    // Fall through to SUCCESS/PARTIAL_SUCCESS handling below
                } else {
                    emit_event(event_bus, EventType::STAGE_FAILED, current->id,
                               "Retries exhausted for: " + current->id);
                    emit_event(event_bus, EventType::PIPELINE_FAILED, "",
                               "Retries exhausted for: " + current->id);
                    return Result<void>::failure("retries exhausted for node: " + current->id);
                }
            }

            // Handle FAILURE -- write stage dir, apply context, then check for recovery edges
            if (outcome.status == StageStatus::FAILURE) {
                // Always write stage directory on failure so logs are available
                if (!config_.logs_root.empty()) {
                    write_stage_directory(*current, outcome);
                }

                // Always apply context updates on failure — parallel handlers store
                // per-branch status (SUCCESS/FAILURE) that the skip logic needs on resume
                ctx.apply_updates(outcome.context_updates);

                // Record last outcome status for conditional handlers (M3 fix)
                ctx.set("needle.last_outcome.status", to_string(outcome.status));

                std::string fail_err = "Stage failed: " + current->id + ": " + outcome.output;
                nlohmann::json fail_data;
                fail_data["error"] = outcome.output;
                fail_data["node_id"] = current->id;
                emit_event(event_bus, EventType::STAGE_FAILED, current->id, fail_err, fail_data);

                // Cycle detection: track failure signatures to catch infinite loops.
                {
                    std::string sig = current->id + "|" + outcome.output.substr(0, 100);
                    failure_signatures_[sig]++;
                    if (failure_signatures_[sig] >= cycle_limit_) {
                        std::string msg = "Cycle detected: node " + current->id + " failed " +
                                          std::to_string(cycle_limit_) + " times with same error";
                        NEEDLE_LOG_ERROR("engine", "%s", msg.c_str());
                        save_checkpoint(current->id, ctx);
                        emit_event(event_bus, EventType::PIPELINE_FAILED, current->id, msg);
                        return Result<void>::failure("cycle detected");
                    }
                }

                bool node_troubleshoot_disabled = false;
                std::string node_troubleshoot = current->attrs.get("troubleshoot");
                if (node_troubleshoot == "false" || node_troubleshoot == "0") {
                    node_troubleshoot_disabled = true;
                }
                if (config_.troubleshoot_mode != TroubleshootMode::Off && !node_troubleshoot_disabled) {
                    AutoTroubleshoot ats;
                    AutoTroubleshootResult ats_result = ats.handle(
                        current->id, exec_ctx.graph, config_.logs_root, ctx,
                        config_.max_attempts_per_stage, config_.troubleshoot_mode,
                        &event_bus);
                    if (ats_result.action == AutoTroubleshootAction::Resumed) {
                        save_checkpoint(current->id, ctx);
                        continue;
                    }
                    if (ats_result.action == AutoTroubleshootAction::Escalated) {
                        save_checkpoint(current->id, ctx);
                        return Result<void>::failure(
                            "escalated auto-troubleshoot: " + ats_result.report_path);
                    }
                }

                // Check if there are outgoing edges with conditions (failure recovery paths)
                auto fail_edges = exec_ctx.graph.outgoing_edges(current->id);
                bool has_conditional_edge = false;
                for (const auto* e : fail_edges) {
                    if (!e->condition().empty()) {
                        has_conditional_edge = true;
                        break;
                    }
                }

                if (has_conditional_edge) {
                    auto edge_result = select_next_edge(*current, outcome, ctx, exec_ctx);
                    if (edge_result.ok()) {
                        // Found a recovery route -- continue pipeline
                        save_checkpoint(current->id, ctx);
                        current = exec_ctx.graph.find_node(edge_result.value()->to);
                        continue;
                    }
                }

                // Failure routing cascade: retry_target -> fallback_retry_target -> graph targets
                {
                    const std::string candidates[] = {
                        current->attrs.get("retry_target"),
                        current->attrs.get("fallback_retry_target"),
                        exec_ctx.graph.graph_attrs().get("retry_target"),
                        exec_ctx.graph.graph_attrs().get("fallback_retry_target")
                    };
                    bool jumped = false;
                    for (const auto& target : candidates) {
                        if (target.empty()) continue;
                        const Node* target_node = exec_ctx.graph.find_node(target);
                        if (target_node) {
                            save_checkpoint(current->id, ctx);
                            current = target_node;
                            jumped = true;
                            break;
                        }
                    }
                    if (jumped) continue;  // re-enter the inner while loop with new current
                }

                // Save checkpoint with updated context (branch statuses) before failing
                save_checkpoint(current->id, ctx);

                // No recovery path -- hard fail
                emit_event(event_bus, EventType::PIPELINE_FAILED, "", fail_err);
                return Result<void>::failure("stage failed: " + current->id + ": " + outcome.output);
            }

            // SUCCESS, PARTIAL_SUCCESS, or SKIP
            emit_event(event_bus, EventType::STAGE_COMPLETED, current->id,
                       "Stage completed: " + current->id);

            // Apply context updates
            ctx.apply_updates(outcome.context_updates);

            // Record last outcome status for conditional handlers (M3 fix)
            ctx.set("needle.last_outcome.status", to_string(outcome.status));

            // Track completed and record outcome (M16: thread-safe)
            record_node_completion(*current, outcome.status);

            // Save checkpoint
            save_checkpoint(current->id, ctx);

            // Write stage directory
            if (config_.auto_status && !config_.logs_root.empty()) {
                write_stage_directory(*current, outcome);
            }

            // After a parallel node, advance normally to the fan-in node. The
            // fan-in runs once in this (parent) context with all parallel.*
            // state populated — not inside each branch with empty state.
            // (Previously we skipped the fan-in because branches ran it
            // inclusive-end; that path is now disabled in parallel_handler.)
            if (current->type == NodeType::PARALLEL) {
                ctx.set("parallel.fan_in_target", "");  // no longer consumed here, clear for cleanliness
            }

            // Select next edge
            auto edge_result = select_next_edge(*current, outcome, ctx, exec_ctx);
            if (!edge_result.ok()) {
                emit_event(event_bus, EventType::PIPELINE_FAILED, "",
                           "Edge selection failed: " + edge_result.error());
                return Result<void>::failure("edge selection failed: " + edge_result.error());
            }

            const Edge* next_edge = edge_result.value();

            // Loop restart: if the selected edge has loop_restart=true, do a fresh restart
            if (next_edge->attrs.get("loop_restart") == "true") {
                NEEDLE_LOG_INFO("engine", "loop restart triggered on edge %s -> %s",
                                next_edge->from.c_str(), next_edge->to.c_str());
                emit_event(event_bus, EventType::STAGE_WARNING, current->id,
                           "Loop restart: resetting pipeline from " + next_edge->to);

                // Build a fresh context preserving only var.* and graph.* keys
                Context fresh_ctx;
                for (const auto& kv : ctx.all()) {
                    if (kv.first.substr(0, 4) == "var." || kv.first.substr(0, 6) == "graph.") {
                        fresh_ctx.set(kv.first, kv.second);
                    }
                }
                ctx = fresh_ctx;

                // Reset execution state
                {
                    std::lock_guard<std::mutex> lock(execution_state_mutex_);
                    completed_nodes_.clear();
                    node_outcomes_.clear();
                }

                // Set current to the target node
                current = session.graph.find_node(next_edge->to);
                if (!current) {
                    emit_event(event_bus, EventType::PIPELINE_FAILED, "",
                               "Loop restart target node not found: " + next_edge->to);
                    return Result<void>::failure("loop restart target not found: " + next_edge->to);
                }
                continue;  // re-enter inner while loop with new current
            }

            current = session.graph.find_node(next_edge->to);
            if (!current) {
                emit_event(event_bus, EventType::PIPELINE_FAILED, "",
                           "Target node not found: " + next_edge->to);
                return Result<void>::failure("target node not found: " + next_edge->to);
            }
        }

        // Inner while finished — either we're at exit, or cancelled.
        // Break out of the for(;;) — the only way back in is via `continue` above.
        break;
    }

    if (cancelled_.load()) {
        emit_event(event_bus, EventType::PIPELINE_FAILED, "", "Pipeline cancelled");
        return Result<void>::failure("pipeline cancelled");
    }

    // Execute exit node
    if (current && current->id == exit_node->id) {
        emit_event(event_bus, EventType::STAGE_STARTED, current->id,
                   "Stage started: " + current->id);
        if (!config_.logs_root.empty()) {
            std::string stage_dir = config_.logs_root + "/stages/" + current->id;
            platform::mkdir_p(stage_dir);
            if (!config_.project_dir.empty()) {
                std::string head = run_command_capture("git -C " + shell_quote(config_.project_dir) + " rev-parse HEAD 2>/dev/null");
                if (!head.empty()) {
                    std::ofstream start_out(stage_dir + "/start_commit.txt");
                    if (start_out.is_open()) start_out << head << "\n";
                }
            }
        }

        Handler* handler = nullptr;
        if (config_.handler_registry) {
            handler = config_.handler_registry->get(current->handler_type());
        }
        if (handler) {
            Result<Outcome> outcome_result = handler->execute(*current, ctx, exec_ctx);
            if (outcome_result.ok()) {
                ctx.apply_updates(outcome_result.value().context_updates);
            }
        }

        emit_event(event_bus, EventType::STAGE_COMPLETED, current->id,
                   "Stage completed: " + current->id);
        record_node_completion(*current, StageStatus::SUCCESS);
        save_checkpoint(current->id, ctx);
    }

    // Check goal gates using node_outcomes_
    {
        std::string unsatisfied_node;
        {
            std::lock_guard<std::mutex> lock(execution_state_mutex_);
            for (const auto& node : session.graph.nodes()) {
                if (!node.goal_gate()) continue;
                auto it = node_outcomes_.find(node.id);
                if (it == node_outcomes_.end() ||
                    (it->second != StageStatus::SUCCESS && it->second != StageStatus::PARTIAL_SUCCESS)) {
                    unsatisfied_node = node.id;
                    break;
                }
            }
        }

        if (!unsatisfied_node.empty()) {
            // Find retry target via cascade: node -> node fallback -> graph -> graph fallback
            const Node* gate_node = session.graph.find_node(unsatisfied_node);
            std::vector<std::string> candidates;
            if (gate_node) {
                candidates.push_back(gate_node->attrs.get("retry_target"));
                candidates.push_back(gate_node->attrs.get("fallback_retry_target"));
            }
            candidates.push_back(session.graph.graph_attrs().get("retry_target"));
            candidates.push_back(session.graph.graph_attrs().get("fallback_retry_target"));

            bool retried = false;
            for (const auto& target : candidates) {
                if (target.empty()) continue;
                const Node* target_node = session.graph.find_node(target);
                if (target_node) {
                    current = target_node;
                    session.current = current;
                    // Re-enter execute_loop via a recursive tail call
                    // (this matches the old goto behavior)
                    return execute_loop(session);
                }
            }
            (void)retried;

            // No retry target found -- fail
            std::string msg = "Goal gate unsatisfied: " + unsatisfied_node;
            emit_event(event_bus, EventType::PIPELINE_FAILED, "", msg);
            return Result<void>::failure(msg);
        }
    }

    // Write manifest
    if (!config_.logs_root.empty()) {
        write_manifest(session.graph, ctx);
    }

    NEEDLE_LOG_INFO("engine", "pipeline completed");
    emit_event(event_bus, EventType::PIPELINE_COMPLETED, "", "Pipeline completed");
    return Result<void>::success();
}

// ── execute_subgraph() ──────────────────────────────────────────

Result<Outcome> PipelineEngine::execute_subgraph(
    const std::string& start_node_id,
    const std::string& end_node_id,
    Context& ctx,
    const ExecutionContext& exec_ctx)
{
    // Use the engine's retry controller (non-parallel path)
    return execute_subgraph(start_node_id, end_node_id, ctx, exec_ctx, nullptr, true);
}

Result<Outcome> PipelineEngine::execute_subgraph(
    const std::string& start_node_id,
    const std::string& end_node_id,
    Context& ctx,
    const ExecutionContext& exec_ctx,
    RetryController* branch_retry)
{
    return execute_subgraph(start_node_id, end_node_id, ctx, exec_ctx, branch_retry, true);
}

Result<Outcome> PipelineEngine::execute_subgraph(
    const std::string& start_node_id,
    const std::string& end_node_id,
    Context& ctx,
    const ExecutionContext& exec_ctx,
    RetryController* branch_retry,
    bool inclusive_end)
{
    // M7: Use branch-local retry controller if provided, else engine's
    RetryController& rc = branch_retry ? *branch_retry : retry_controller_;

    const Node* current = exec_ctx.graph.find_node(start_node_id);
    if (!current) {
        return Result<Outcome>::failure("start node not found: " + start_node_id);
    }

    Outcome last_outcome;
    last_outcome.status = StageStatus::SUCCESS;

    while (current && current->id != end_node_id && !exec_ctx.cancelled.load()) {
        emit_event(exec_ctx.event_bus, EventType::STAGE_STARTED, current->id,
                   "Stage started: " + current->id);
        if (!config_.logs_root.empty()) {
            std::string stage_dir = config_.logs_root + "/stages/" + current->id;
            platform::mkdir_p(stage_dir);
            if (!config_.project_dir.empty()) {
                std::string head = run_command_capture("git -C " + shell_quote(config_.project_dir) + " rev-parse HEAD 2>/dev/null");
                if (!head.empty()) {
                    std::ofstream start_out(stage_dir + "/start_commit.txt");
                    if (start_out.is_open()) start_out << head << "\n";
                }
            }
        }

        Handler* handler = nullptr;
        if (config_.handler_registry) {
            handler = config_.handler_registry->get(current->handler_type());
        }
        if (!handler) {
            emit_event(exec_ctx.event_bus, EventType::STAGE_FAILED, current->id,
                       "No handler for type: " + current->handler_type());
            return Result<Outcome>::failure("no handler for type: " + current->handler_type());
        }

        Result<Outcome> outcome_result = handler->execute(*current, ctx, exec_ctx);
        if (!outcome_result.ok()) {
            NEEDLE_LOG_ERROR("subgraph", "node %s: handler returned error: %s",
                             current->id.c_str(), outcome_result.error().c_str());
            emit_event(exec_ctx.event_bus, EventType::STAGE_FAILED, current->id,
                       "Handler error: " + outcome_result.error());
            return outcome_result;
        }

        Outcome outcome = outcome_result.value();
        NEEDLE_LOG_DEBUG("subgraph", "node %s: handler returned status=%s output_len=%zu",
                         current->id.c_str(), to_string(outcome.status).c_str(),
                         outcome.output.size());

        if (outcome.status == StageStatus::FAILURE) {
            if (!config_.logs_root.empty()) {
                write_stage_directory(*current, outcome);
            }
            emit_event(exec_ctx.event_bus, EventType::STAGE_FAILED, current->id,
                       "Stage failed: " + current->id + ": " + outcome.output);
            // M16: record completion even on failure
            record_node_completion(*current, outcome.status);
            return Result<Outcome>::success(std::move(outcome));
        }

        if (outcome.status == StageStatus::RETRY) {
            RetryPolicy policy = RetryPolicy::from_attributes(current->attrs);
            if (rc.should_retry(current->id, policy)) {
                rc.record_attempt(current->id);
                emit_event(exec_ctx.event_bus, EventType::STAGE_RETRYING, current->id,
                           "Retrying: " + current->id);
                rc.sleep_before_retry(current->id, policy);
                continue;
            }
            Outcome fail;
            fail.status = StageStatus::FAILURE;
            fail.output = "retries exhausted for node: " + current->id;
            emit_event(exec_ctx.event_bus, EventType::STAGE_FAILED, current->id, fail.output);
            return Result<Outcome>::success(std::move(fail));
        }

        emit_event(exec_ctx.event_bus, EventType::STAGE_COMPLETED, current->id,
                   "Stage completed: " + current->id);

        ctx.apply_updates(outcome.context_updates);

        // M16: record completion in parent's state under mutex
        record_node_completion(*current, outcome.status);

        // Write stage directory for subgraph nodes too
        if (config_.auto_status && !config_.logs_root.empty()) {
            write_stage_directory(*current, outcome);
        }

        last_outcome = outcome;

        auto edge_result = select_next_edge(*current, outcome, ctx, exec_ctx);
        if (!edge_result.ok()) {
            return Result<Outcome>::failure("edge selection failed: " + edge_result.error());
        }

        current = exec_ctx.graph.find_node(edge_result.value()->to);
    }

    // M13: Inclusive end-node execution — execute the end node before returning.
    // Skipped when inclusive_end=false (e.g. parallel branches that stop before
    // the fan-in, so the fan-in runs once in the parent context instead of
    // N times inside branches with incomplete parallel.* state).
    if (inclusive_end && current && current->id == end_node_id && !exec_ctx.cancelled.load()) {
        emit_event(exec_ctx.event_bus, EventType::STAGE_STARTED, current->id,
                   "Stage started: " + current->id);
        if (!config_.logs_root.empty()) {
            std::string stage_dir = config_.logs_root + "/stages/" + current->id;
            platform::mkdir_p(stage_dir);
            if (!config_.project_dir.empty()) {
                std::string head = run_command_capture("git -C " + shell_quote(config_.project_dir) + " rev-parse HEAD 2>/dev/null");
                if (!head.empty()) {
                    std::ofstream start_out(stage_dir + "/start_commit.txt");
                    if (start_out.is_open()) start_out << head << "\n";
                }
            }
        }

        Handler* handler = nullptr;
        if (config_.handler_registry) {
            handler = config_.handler_registry->get(current->handler_type());
        }
        if (!handler) {
            emit_event(exec_ctx.event_bus, EventType::STAGE_FAILED, current->id,
                       "No handler for type: " + current->handler_type());
            return Result<Outcome>::failure("no handler for type: " + current->handler_type());
        }

        Result<Outcome> outcome_result = handler->execute(*current, ctx, exec_ctx);
        if (!outcome_result.ok()) {
            NEEDLE_LOG_ERROR("subgraph", "end node %s: handler returned error: %s",
                             current->id.c_str(), outcome_result.error().c_str());
            emit_event(exec_ctx.event_bus, EventType::STAGE_FAILED, current->id,
                       "Handler error: " + outcome_result.error());
            return outcome_result;
        }

        Outcome outcome = outcome_result.value();
        if (outcome.status == StageStatus::FAILURE) {
            if (!config_.logs_root.empty()) {
                write_stage_directory(*current, outcome);
            }
            emit_event(exec_ctx.event_bus, EventType::STAGE_FAILED, current->id,
                       "Stage failed: " + current->id + ": " + outcome.output);
            record_node_completion(*current, outcome.status);
            return Result<Outcome>::success(std::move(outcome));
        }

        emit_event(exec_ctx.event_bus, EventType::STAGE_COMPLETED, current->id,
                   "Stage completed: " + current->id);
        ctx.apply_updates(outcome.context_updates);
        record_node_completion(*current, outcome.status);

        if (config_.auto_status && !config_.logs_root.empty()) {
            write_stage_directory(*current, outcome);
        }

        last_outcome = outcome;
    }

    return Result<Outcome>::success(std::move(last_outcome));
}

Result<Outcome> PipelineEngine::execute_node(
    const Node& node, Context& ctx, const ExecutionContext& exec_ctx)
{
    Handler* handler = nullptr;
    if (config_.handler_registry) {
        handler = config_.handler_registry->get(node.handler_type());
    }
    if (!handler) {
        return Result<Outcome>::failure("no handler for type: " + node.handler_type());
    }
    return handler->execute(node, ctx, exec_ctx);
}

Result<const Edge*> PipelineEngine::select_next_edge(
    const Node& node, const Outcome& outcome,
    const Context& ctx, const ExecutionContext& exec_ctx)
{
    auto candidates = exec_ctx.graph.outgoing_edges(node.id);
    if (candidates.empty()) {
        return Result<const Edge*>::failure("no outgoing edges from node: " + node.id);
    }

    if (config_.edge_selector) {
        return config_.edge_selector->select(candidates, outcome, ctx);
    }

    // Default: use built-in selector
    EdgeSelector selector;
    return selector.select(candidates, outcome, ctx);
}

void PipelineEngine::save_checkpoint(const std::string& current_node, const Context& ctx) {
    if (!config_.checkpoint_writer) return;

    Checkpoint cp;
    cp.timestamp = utc_timestamp_now();
    cp.current_node = current_node;

    // M16: acquire mutex when reading shared state
    {
        std::lock_guard<std::mutex> lock(execution_state_mutex_);
        cp.completed_nodes = completed_nodes_;
        cp.completed_node_hashes = completed_node_hashes_;
    }

    cp.context = ctx.clone();
    for (const auto& kv : ctx.all()) {
        const std::string prefix = "needle.branch_worktree.";
        if (kv.first.size() > prefix.size() &&
            kv.first.compare(0, prefix.size(), prefix) == 0) {
            cp.branch_worktrees[kv.first.substr(prefix.size())] = kv.second;
        }
    }
    cp.graph_file = config_.graph_file;
    cp.graph_hash = current_graph_hash_;
    cp.stylesheet_file = config_.stylesheet_file;
    cp.logs_root = config_.logs_root;
    cp.dot_content_hash = config_.dot_content_hash;

    // Copy retry state
    nlohmann::json rc_json = retry_controller_.to_json();
    for (auto it = rc_json.begin(); it != rc_json.end(); ++it) {
        cp.retry_counters[it.key()] = it.value().get<int>();
    }

    std::string path = config_.logs_root.empty() ? "checkpoint.json"
                       : config_.logs_root + "/checkpoint.json";
    auto result = config_.checkpoint_writer->save(cp, path);
    if (!result.ok()) {
        NEEDLE_LOG_ERROR("checkpoint", "FAILED to save checkpoint for node %s: %s",
                         current_node.c_str(), result.error().c_str());
    } else {
        NEEDLE_LOG_INFO("checkpoint", "saved checkpoint: node=%s completed=%zu",
                        current_node.c_str(), completed_nodes_.size());
    }
}

void PipelineEngine::check_goal_gates(const Graph& graph, const Context& ctx, Diagnostics& diags) {
    for (const auto& node : graph.nodes()) {
        if (node.goal_gate()) {
            // Check if the goal condition is satisfied in context
            std::string goal_key = "goal." + node.id;
            if (!ctx.has(goal_key) || ctx.get(goal_key) != "satisfied") {
                Diagnostic d;
                d.severity = DiagnosticSeverity::Error;
                d.code = "GOAL";
                d.message = "Goal gate unsatisfied for node: " + node.id;
                d.node_id = node.id;
                diags.add(std::move(d));
            }
        }
    }
}

void PipelineEngine::write_stage_directory(const Node& node, const Outcome& outcome) {
    std::string dir = config_.logs_root + "/stages/" + node.id;
    platform::mkdir_p(dir);

    // Don't archive_stage_files here. Handlers that produce real artifacts
    // (cli_backend writes response.md from process stdout; interactive_handler
    // writes response.md from the user's reply) call archive_stage_files at
    // the START of their own execution, when the previous run's files are
    // still on disk. Archiving again here rotates the FRESHLY-written file
    // we just got from the handler — which we then overwrite below with
    // outcome.output, losing the agent's actual stdout on timeouts/failures.

    // Write status.json — engine's authoritative summary of this execution.
    nlohmann::json status;
    status["node_id"] = node.id;
    status["status"] = to_string(outcome.status);
    status["output"] = outcome.output;
    status["preferred_label"] = outcome.preferred_label;

    nlohmann::json updates = nlohmann::json::object();
    for (const auto& kv : outcome.context_updates) {
        updates[kv.first] = kv.second;
    }
    status["context_updates"] = updates;

    // N2: Promote any `<handler>.<node>.git_state` context update to a
    // top-level `git_state` field. Operators looking at status.json see the
    // commits/files that landed during the stage as a structured object
    // rather than an opaque JSON-in-string under context_updates.
    {
        std::string git_state_key = std::string();
        // Look for any key matching the suffix `.git_state`.
        for (const auto& kv : outcome.context_updates) {
            const std::string suffix = ".git_state";
            if (kv.first.size() >= suffix.size() &&
                kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) == 0) {
                git_state_key = kv.first;
                break;
            }
        }
        if (!git_state_key.empty()) {
            try {
                status["git_state"] = nlohmann::json::parse(outcome.context_updates.at(git_state_key));
            } catch (...) {
                // Leave the string-form in context_updates; don't fail the
                // status write over a malformed git_state entry.
            }
        }
    }

    // Surface timeout_kind at top level too — its primary consumer
    // (troubleshoot agent) needs it without parsing context updates.
    for (const auto& kv : outcome.context_updates) {
        const std::string suffix = ".timeout_kind";
        if (kv.first.size() >= suffix.size() &&
            kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) == 0) {
            status["timeout_kind"] = kv.second;
            break;
        }
    }
    for (const auto& kv : outcome.context_updates) {
        const std::string suffix = ".cherry_pick_conflict";
        if (kv.first.size() >= suffix.size() &&
            kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) == 0) {
            try {
                status["cherry_pick_conflict"] = nlohmann::json::parse(kv.second);
            } catch (...) {
                status["cherry_pick_conflict"] = kv.second;
            }
            break;
        }
    }

    std::string status_path = dir + "/status.json";
    std::ofstream out(status_path);
    if (out.is_open()) {
        out << status.dump(2);
    }

    // Write prompt.md if node has a prompt AND the handler didn't already write one
    std::string prompt = node.prompt();
    if (!prompt.empty()) {
        std::string prompt_path = dir + "/prompt.md";
        if (!platform::file_exists(prompt_path)) {
            std::ofstream pout(prompt_path);
            if (pout.is_open()) {
                pout << prompt;
            }
        }
    }

    // Write response.md only if the handler didn't already write one. This
    // preserves cli_backend's process stdout (the agent's actual output)
    // even on timeout/failure, so the user can see what the agent did.
    // status.json carries outcome.output for the engine's summary.
    std::string resp_path = dir + "/response.md";
    if (!outcome.output.empty() && !platform::file_exists(resp_path)) {
        std::ofstream rout(resp_path);
        if (rout.is_open()) {
            rout << outcome.output;
        }
    }
}

void PipelineEngine::write_manifest(const Graph& graph, const Context& ctx) {
    (void)ctx;
    std::string end_time = utc_timestamp_now();

    nlohmann::json manifest;
    manifest["graph_name"] = graph.name();
    manifest["graph_file"] = config_.graph_file;
    manifest["start_time"] = start_time_;
    manifest["end_time"] = end_time;
    manifest["nodes_visited"] = completed_nodes_;
    manifest["node_count"] = completed_nodes_.size();

    std::string dir = config_.logs_root;
    platform::mkdir_p(dir);

    std::string path = dir + "/manifest.json";
    std::ofstream out(path);
    if (out.is_open()) {
        out << manifest.dump(2);
    }
}

} // namespace needle
