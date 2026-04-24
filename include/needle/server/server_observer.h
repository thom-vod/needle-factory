#pragma once

#include <string>

namespace needle {

struct PipelineEvent;

/// Extension point for the HTTP server.
///
/// External code (for example, a metrics emitter or an orchestration
/// integration) can subclass ServerObserver and register an instance via
/// NeedleHttpServer::add_observer before calling start(). The server will
/// dispatch lifecycle and per-run events to every registered observer.
///
/// All methods have empty default implementations so subclasses override only
/// the hooks they care about. Methods may be invoked from any thread, so
/// implementations must be thread-safe.
class ServerObserver {
public:
    virtual ~ServerObserver() = default;

    /// Called for each pipeline event on each run, after SSE dispatch.
    virtual void on_run_event(const std::string& /*run_id*/,
                              const PipelineEvent& /*event*/) {}

    /// Called when the last active run completes (active run count drops
    /// from 1 to 0).
    virtual void on_pipeline_idle() {}

    /// Called after the HTTP server has bound and is about to begin
    /// accepting connections. `url` is the full bind address.
    virtual void on_server_started(const std::string& /*url*/) {}

    /// Called during shutdown, after the HTTP listener thread has joined.
    virtual void on_server_stopped() {}

    /// Called when startup fails (for example, bind error) or a pipeline
    /// encounters a fatal error.
    virtual void on_error(const std::string& /*message*/) {}
};

} // namespace needle
