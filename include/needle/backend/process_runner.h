#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <queue>
#include "needle/model/result.h"

namespace needle {

// Distinguishes the cause of a timeout:
// - None: not timed out
// - WallClock: total elapsed time exceeded `timeout_ms`
// - Idle: no stdout/stderr output observed for `idle_timeout_ms`. Surfaces
//   stalled agent sessions much faster than wall-clock alone — a wedged
//   process is killed within ~idle_timeout_ms of its last output line.
enum class TimeoutKind {
    None,
    WallClock,
    Idle,
};

struct ProcessResult {
    int exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;
    bool timed_out = false;
    TimeoutKind timeout_kind = TimeoutKind::None;
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() {}
    // `idle_timeout_ms = 0` disables idle-output tracking (legacy behaviour).
    // Set to a positive value to kill the child if no stdout/stderr arrives
    // for that many milliseconds.
    virtual Result<ProcessResult> run(
        const std::string& command,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        int timeout_ms,
        const std::map<std::string, std::string>& env_overrides = std::map<std::string, std::string>(),
        const std::string& stdin_data = "",
        int idle_timeout_ms = 0
    ) = 0;

    virtual void kill_all() {}
};

#ifndef _WIN32

class PosixProcessRunner : public ProcessRunner {
public:
    Result<ProcessResult> run(
        const std::string& command,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        int timeout_ms,
        const std::map<std::string, std::string>& env_overrides = std::map<std::string, std::string>(),
        const std::string& stdin_data = "",
        int idle_timeout_ms = 0
    ) override;

    void kill_all() override;

private:
    static bool is_sensitive_env_var(const std::string& name);
    std::mutex pids_mutex_;
    std::vector<pid_t> active_pids_;
};

using NativeProcessRunner = PosixProcessRunner;

#else // _WIN32

class Win32ProcessRunner : public ProcessRunner {
public:
    Result<ProcessResult> run(
        const std::string& command,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        int timeout_ms,
        const std::map<std::string, std::string>& env_overrides = std::map<std::string, std::string>(),
        const std::string& stdin_data = "",
        int idle_timeout_ms = 0
    ) override;

    void kill_all() override;

private:
    static bool is_sensitive_env_var(const std::string& name);

    struct ActiveProcess {
        void* process;  // HANDLE
        void* job;      // HANDLE
    };
    std::mutex procs_mutex_;
    std::vector<ActiveProcess> active_procs_;
};

using NativeProcessRunner = Win32ProcessRunner;

#endif // _WIN32

struct MockResponse {
    ProcessResult result;
};

class MockProcessRunner : public ProcessRunner {
public:
    void enqueue(ProcessResult response);

    Result<ProcessResult> run(
        const std::string& command,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        int timeout_ms,
        const std::map<std::string, std::string>& env_overrides = std::map<std::string, std::string>(),
        const std::string& stdin_data = "",
        int idle_timeout_ms = 0
    ) override;

    struct CallRecord {
        std::string command;
        std::vector<std::string> args;
        std::string working_dir;
        int timeout_ms;
        std::map<std::string, std::string> env_overrides;
        std::string stdin_data;
        int idle_timeout_ms;
    };

    std::vector<CallRecord> calls() const;

private:
    mutable std::mutex mutex_;
    std::queue<ProcessResult> responses_;
    std::vector<CallRecord> calls_;
};

} // namespace needle
