#pragma once

#ifdef NEEDLE_ENABLE_SERVER

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>

#include "needle/model/graph.h"
#include "needle/model/context.h"
#include "needle/model/result.h"
#include "needle/engine/pipeline_engine.h"
#include "needle/event/event_bus.h"
#include "needle/event/collector_event_bus.h"
#include "needle/server/dot_generator.h"
#include "needle/interviewer/interviewer.h"
#include "needle/handlers/interactive_session.h"
#include "needle/engine/pause_controller.h"
#include "needle/util/run_registry.h"
#include "needle/server/server_observer.h"

namespace needle {

// Append-only global event queue for the global SSE endpoint
struct GlobalEventQueue {
    mutable std::mutex mutex;
    std::vector<std::string> events;  // formatted SSE data lines
    size_t sequence = 0;              // monotonic counter
};

struct PipelineRun {
    std::string id;
    std::string dot_source;  // DOT used for this run (empty = startup graph)
    std::string dot_stem;    // filesystem-safe stem, e.g. "weather__air_quality_dashboard"
    std::string project_dir; // absolute project directory
    std::string created_at;  // ISO 8601 creation timestamp
    std::atomic<bool> cancelled;
    CollectorEventBus collector;
    EventBus event_bus;
    std::thread run_thread;
    std::shared_ptr<Interviewer> interviewer;  // per-run interviewer (HttpInterviewer for serve)
    std::shared_ptr<InteractiveSession> interactive_session;
    std::string logs_root;  // per-run logs directory (project_dir/.needle/<dot_stem>)
    bool dry_run = false;
    mutable size_t cached_total_stages = 0;  // cached for derive_run_view

    PipelineRun() : cancelled(false) {}

    // M9: Safety net — join thread if still joinable on destruction
    ~PipelineRun() {
        if (run_thread.joinable()) {
            cancelled.store(true);
            run_thread.join();
        }
    }

    // Non-copyable due to atomic/mutex/thread members
    PipelineRun(const PipelineRun&) = delete;
    PipelineRun& operator=(const PipelineRun&) = delete;

    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = s;
    }
    void set_error(const std::string& s) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        error_ = s;
    }
    std::string get_status() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_;
    }
    std::string get_error() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return error_;
    }

private:
    mutable std::mutex status_mutex_;
    std::string status_;    // "running", "completed", "failed", "cancelled"
    std::string error_;
};

class NeedleHttpServer {
public:
    NeedleHttpServer(int port, const std::string& bind_addr = "127.0.0.1");
    ~NeedleHttpServer();

    void start(const Graph& graph, PipelineConfig config, EventBus& global_bus);
    void stop();

    /// Register an observer. Must be called before start(). The server
    /// keeps shared ownership for the duration of its lifetime.
    void add_observer(std::shared_ptr<ServerObserver> observer);

    /// Block until the HTTP server is ready to accept connections.
    void wait_until_ready();

    /// Disable run persistence (for tests). Call before start().
    void disable_run_persistence() { run_registry_->set_enabled(false); }

private:
    int port_;
    std::string bind_addr_;
    std::thread server_thread_;
    std::atomic<bool> running_;

    // Stop function for the HTTP server (type-erased)
    std::function<void()> stop_fn_;

    // Shared state
    mutable std::mutex runs_mutex_;
    std::map<std::string, std::shared_ptr<PipelineRun>> runs_;
    Graph graph_;
    PipelineConfig config_;

    // Cached graph artifacts (computed once at startup)
    std::string cached_dot_;
    std::string cached_svg_;
    std::string cached_page_;

    // Global SSE event queue
    std::shared_ptr<GlobalEventQueue> global_queue_;

    // Global pause controller — shared with all engine threads
    std::shared_ptr<PauseController> pause_controller_ = std::make_shared<PauseController>();
    std::thread pause_timer_thread_;  // background thread for scheduled resume

    // Answer queue for HTTP interviewer
    mutable std::mutex answer_mutex_;
    std::condition_variable answer_cv_;
    std::map<std::string, std::string> pending_answers_;  // run_id -> answer

    struct TroubleshootInFlight {
        bool active = false;
        std::string session_id;
        int agent_pid = 0;
        std::shared_ptr<ProcessRunner> runner;
    };
    mutable std::mutex troubleshoot_mutex_;
    std::map<std::string, TroubleshootInFlight> troubleshoot_in_flight_;

    DotGenerator dot_generator_;

    std::string generate_run_id(const std::string& project_dir = "");

    // Start a run with given graph, returning the run JSON response.
    // stem_override: if non-empty, use this stem instead of deriving from
    // dot_source's graph label. Used when the user's on-disk filename
    // differs from the label so logs_root lines up with that filename.
    // graph_file: canonical on-disk path of the DOT — recorded in the
    // engine's checkpoint so resume can find the source again later.
    std::shared_ptr<PipelineRun> create_run(const Graph& run_graph,
                                             const std::string& dot_source,
                                             const std::string& project_dir = ".",
                                             const std::map<std::string, std::string>& vars = {},
                                             const std::string& stem_override = "",
                                             const std::string& graph_file = "");

    // Derive display state from a run's event history
    nlohmann::json derive_run_view(const PipelineRun& run) const;

    // Reconstruct run view from disk (for persisted runs after restart)
    nlohmann::json reconstruct_run_view_from_disk(const PipelineRun& run) const;

    // Compute per-DOT logs root
    static std::string compute_logs_root(const std::string& project_dir, const std::string& dot_stem);

    // Migrate flat .needle/ to per-DOT subdirectory
    static bool migrate_flat_needle_dir(const std::string& project_dir, const std::string& dot_stem);

    std::shared_ptr<RunRegistry> run_registry_ = std::make_shared<RunRegistry>();
    std::atomic<int> active_runs_{0};
    std::shared_ptr<void> svr_ptr_;  // type-erased to avoid httplib.h in header

    std::vector<std::shared_ptr<ServerObserver>> observers_;
    void notify_run_event(const std::string& run_id, const PipelineEvent& event);
    void notify_idle();
    void notify_started(const std::string& url);
    void notify_stopped();
    void notify_error(const std::string& message);
};

} // namespace needle

#endif // NEEDLE_ENABLE_SERVER
