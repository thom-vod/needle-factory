#include "needle/backend/process_runner.h"

#ifndef _WIN32

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

extern char** environ;

namespace needle {

namespace {

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

bool ends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // anonymous namespace

bool PosixProcessRunner::is_sensitive_env_var(const std::string& name) {
    // Filter *_API_KEY, *_SECRET, *_TOKEN
    return ends_with(name, "_API_KEY") ||
           ends_with(name, "_SECRET") ||
           ends_with(name, "_TOKEN");
}

Result<ProcessResult> PosixProcessRunner::run(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    int timeout_ms,
    const std::map<std::string, std::string>& env_overrides,
    const std::string& stdin_data)
{
    // Create pipes for stdout and stderr
    int stdout_pipe[2];
    int stderr_pipe[2];
    int stdin_pipe[2] = {-1, -1};

    if (pipe(stdout_pipe) != 0) {
        return Result<ProcessResult>::failure("failed to create stdout pipe");
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return Result<ProcessResult>::failure("failed to create stderr pipe");
    }
    if (!stdin_data.empty()) {
        if (pipe(stdin_pipe) != 0) {
            close(stdout_pipe[0]); close(stdout_pipe[1]);
            close(stderr_pipe[0]); close(stderr_pipe[1]);
            return Result<ProcessResult>::failure("failed to create stdin pipe");
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        return Result<ProcessResult>::failure("fork failed");
    }

    if (pid == 0) {
        // Child process
        setpgid(0, 0);

        // Close read ends of output pipes
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        // Set up stdin: pipe if we have data, /dev/null otherwise
        if (stdin_pipe[0] >= 0) {
            close(stdin_pipe[1]); // Close write end in child
            dup2(stdin_pipe[0], STDIN_FILENO);
            close(stdin_pipe[0]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }

        // Redirect stdout/stderr
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Change working directory
        if (!working_dir.empty()) {
            if (chdir(working_dir.c_str()) != 0) {
                _exit(127);
            }
        }

        // Build environment: inherit all vars, apply overrides
        std::vector<std::string> env_strings;
        if (environ) {
            for (char** ep = environ; *ep; ++ep) {
                std::string entry(*ep);
                size_t eq = entry.find('=');
                if (eq != std::string::npos) {
                    std::string var_name = entry.substr(0, eq);
                    // Skip if overridden (we'll add the override value below)
                    if (env_overrides.find(var_name) != env_overrides.end()) {
                        continue;
                    }
                }
                env_strings.push_back(entry);
            }
        }

        // Add overrides
        for (const auto& kv : env_overrides) {
            env_strings.push_back(kv.first + "=" + kv.second);
        }

        // Build envp
        std::vector<char*> envp;
        envp.reserve(env_strings.size() + 1);
        for (auto& s : env_strings) {
            envp.push_back(&s[0]);
        }
        envp.push_back(nullptr);

        // Build argv: command + args
        std::vector<std::string> argv_strings;
        argv_strings.push_back(command);
        for (const auto& a : args) {
            argv_strings.push_back(a);
        }

        std::vector<char*> argv;
        argv.reserve(argv_strings.size() + 1);
        for (auto& s : argv_strings) {
            argv.push_back(&s[0]);
        }
        argv.push_back(nullptr);

        execve(command.c_str(), argv.data(), envp.data());

        // If execve fails, try execvp with PATH lookup
        execvp(command.c_str(), argv.data());

        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Track active child PID for cancellation
    {
        std::lock_guard<std::mutex> lock(pids_mutex_);
        active_pids_.push_back(pid);
    }

    // Write stdin data to the child if provided, then close
    if (stdin_pipe[1] >= 0) {
        close(stdin_pipe[0]); // Close read end in parent
        const char* ptr = stdin_data.data();
        size_t remaining = stdin_data.size();
        while (remaining > 0) {
            ssize_t n = write(stdin_pipe[1], ptr, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            ptr += n;
            remaining -= static_cast<size_t>(n);
        }
        close(stdin_pipe[1]);
    }

    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);

    // Ensure child is in its own process group
    setpgid(pid, pid);

    std::string stdout_data;
    std::string stderr_data;
    bool timed_out = false;
    bool child_reaped = false;
    int reaped_status = 0;

    struct pollfd fds[2];
    fds[0].fd = stdout_pipe[0];
    fds[0].events = POLLIN;
    fds[1].fd = stderr_pipe[0];
    fds[1].events = POLLIN;

    int open_fds = 2;

    // Use wall-clock time for accurate timeout tracking
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    auto elapsed_ms = [&start_time]() -> int {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<int>(
            (now.tv_sec - start_time.tv_sec) * 1000 +
            (now.tv_nsec - start_time.tv_nsec) / 1000000);
    };

    while (open_fds > 0) {
        int remaining_ms = -1;
        if (timeout_ms > 0) {
            remaining_ms = timeout_ms - elapsed_ms();
            if (remaining_ms <= 0) {
                timed_out = true;
                break;
            }
        }

        // Poll with at most 500 ms so we can interleave waitpid(WNOHANG)
        // checks and notice when the direct child exits even if descendants
        // keep the pipes open (e.g. a backgrounded vite inheriting stdio).
        int poll_timeout = remaining_ms;
        if (poll_timeout < 0 || poll_timeout > 500) {
            poll_timeout = 500;
        }

        int ret = poll(fds, 2, poll_timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Check wall-clock timeout after poll returns
        if (timeout_ms > 0 && elapsed_ms() >= timeout_ms) {
            timed_out = true;
            break;
        }

        char buf[4096];
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0) continue;
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    if (i == 0) {
                        stdout_data.append(buf, static_cast<size_t>(n));
                    } else {
                        stderr_data.append(buf, static_cast<size_t>(n));
                    }
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    --open_fds;
                }
            }
            if (fds[i].fd >= 0 && (fds[i].revents & (POLLERR | POLLNVAL))) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open_fds;
            }
        }

        // Has the direct child exited? If so, stop waiting for the pipes —
        // grandchildren (detached background jobs, daemonized helpers) may
        // keep the write ends open indefinitely. We'll do one more
        // non-blocking drain below to capture any buffered data.
        if (!child_reaped) {
            pid_t w = waitpid(pid, &reaped_status, WNOHANG);
            if (w == pid) {
                child_reaped = true;
                break;
            }
        }
    }

    if (timed_out && !child_reaped) {
        // Send SIGTERM to process group
        kill(-pid, SIGTERM);
        // Wait 2 seconds then SIGKILL
        usleep(2000000);
        kill(-pid, SIGKILL);
    }

    // Close any remaining fds
    if (fds[0].fd >= 0) {
        // Read remaining data
        char buf[4096];
        for (;;) {
            ssize_t n = read(fds[0].fd, buf, sizeof(buf));
            if (n > 0) stdout_data.append(buf, static_cast<size_t>(n));
            else break;
        }
        close(fds[0].fd);
    }
    if (fds[1].fd >= 0) {
        char buf[4096];
        for (;;) {
            ssize_t n = read(fds[1].fd, buf, sizeof(buf));
            if (n > 0) stderr_data.append(buf, static_cast<size_t>(n));
            else break;
        }
        close(fds[1].fd);
    }

    // Wait for child (skip if we already reaped via WNOHANG in the loop)
    int status = reaped_status;
    if (!child_reaped) {
        waitpid(pid, &status, 0);
    }

    // Remove from active PID list
    {
        std::lock_guard<std::mutex> lock(pids_mutex_);
        active_pids_.erase(
            std::remove(active_pids_.begin(), active_pids_.end(), pid),
            active_pids_.end());
    }

    ProcessResult result;
    result.timed_out = timed_out;
    result.stdout_output = std::move(stdout_data);
    result.stderr_output = std::move(stderr_data);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = -1;
    }

    return Result<ProcessResult>::success(std::move(result));
}

void PosixProcessRunner::kill_all() {
    std::lock_guard<std::mutex> lock(pids_mutex_);
    for (pid_t pid : active_pids_) {
        // Kill the process group (child + its children)
        kill(-pid, SIGTERM);
    }
    // Give processes a moment to exit, then force-kill
    if (!active_pids_.empty()) {
        usleep(500000);  // 500ms
        for (pid_t pid : active_pids_) {
            kill(-pid, SIGKILL);
        }
    }
}

} // namespace needle

#endif // !_WIN32

namespace needle {

// MockProcessRunner

void MockProcessRunner::enqueue(ProcessResult response) {
    std::lock_guard<std::mutex> lock(mutex_);
    responses_.push(std::move(response));
}

Result<ProcessResult> MockProcessRunner::run(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    int timeout_ms,
    const std::map<std::string, std::string>& env_overrides,
    const std::string& stdin_data)
{
    std::lock_guard<std::mutex> lock(mutex_);

    CallRecord rec;
    rec.command = command;
    rec.args = args;
    rec.working_dir = working_dir;
    rec.timeout_ms = timeout_ms;
    rec.env_overrides = env_overrides;
    rec.stdin_data = stdin_data;
    calls_.push_back(std::move(rec));

    if (responses_.empty()) {
        return Result<ProcessResult>::failure("no more mock responses");
    }

    ProcessResult resp = std::move(responses_.front());
    responses_.pop();
    return Result<ProcessResult>::success(std::move(resp));
}

std::vector<MockProcessRunner::CallRecord> MockProcessRunner::calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
}

} // namespace needle
