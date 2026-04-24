#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <queue>
#include "needle/model/result.h"

namespace needle {

struct ProcessResult {
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
    bool timed_out;
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() {}
    virtual Result<ProcessResult> run(
        const std::string& command,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        int timeout_ms,
        const std::map<std::string, std::string>& env_overrides = std::map<std::string, std::string>(),
        const std::string& stdin_data = ""
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
        const std::string& stdin_data = ""
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
        const std::string& stdin_data = ""
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
        const std::string& stdin_data = ""
    ) override;

    struct CallRecord {
        std::string command;
        std::vector<std::string> args;
        std::string working_dir;
        int timeout_ms;
        std::map<std::string, std::string> env_overrides;
        std::string stdin_data;
    };

    std::vector<CallRecord> calls() const;

private:
    mutable std::mutex mutex_;
    std::queue<ProcessResult> responses_;
    std::vector<CallRecord> calls_;
};

} // namespace needle
