#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "needle/backend/process_runner.h"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <vector>

namespace needle {

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ends_with_ci(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char a = static_cast<unsigned char>(s[s.size() - n + i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

// Result of resolving a bare command against PATH + PATHEXT.
struct ResolvedExe {
    std::string path;     // Absolute path to the resolved file
    bool is_batch;        // True for .cmd / .bat — must be launched via cmd.exe
    bool ok;
};

std::vector<std::string> parse_pathext() {
    // CreateProcess does not honor PATHEXT. We replicate the cmd.exe/PowerShell
    // behavior of trying each extension in order.
    char buf[4096];
    DWORD n = GetEnvironmentVariableA("PATHEXT", buf, sizeof(buf));
    std::string raw = (n == 0 || n >= sizeof(buf))
        ? std::string(".COM;.EXE;.BAT;.CMD")
        : std::string(buf, n);
    std::vector<std::string> exts;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t pos = raw.find(';', start);
        std::string tok = raw.substr(start, pos == std::string::npos
                                              ? std::string::npos
                                              : pos - start);
        if (!tok.empty() && tok[0] == '.') exts.push_back(tok);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    if (exts.empty()) {
        exts = {".COM", ".EXE", ".BAT", ".CMD"};
    }
    return exts;
}

std::string search_path(const std::string& name, const char* ext) {
    char buf[MAX_PATH];
    DWORD n = SearchPathA(nullptr, name.c_str(), ext,
                          static_cast<DWORD>(sizeof(buf)), buf, nullptr);
    if (n == 0 || n >= sizeof(buf)) return "";
    return std::string(buf, n);
}

bool is_batch_path(const std::string& path) {
    return ends_with_ci(path, ".cmd") || ends_with_ci(path, ".bat");
}

// Resolve a command (possibly bare, e.g. "codex") to an absolute path, walking
// PATH and (if needed) PATHEXT. This mirrors what cmd.exe / PowerShell / bash
// do, which is what users expect — CreateProcess itself only appends ".exe"
// and would fail to find "codex.cmd".
ResolvedExe resolve_executable(const std::string& command) {
    ResolvedExe r{"", false, false};
    if (command.empty()) return r;

    // Does the basename carry an extension already?
    std::string basename = command;
    auto slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    bool has_ext = basename.find('.') != std::string::npos;

    if (has_ext) {
        std::string p = search_path(command, nullptr);
        if (!p.empty()) {
            r.path = p;
            r.is_batch = is_batch_path(p);
            r.ok = true;
        }
        return r;
    }

    for (const auto& ext : parse_pathext()) {
        std::string p = search_path(command, ext.c_str());
        if (!p.empty()) {
            r.path = p;
            r.is_batch = is_batch_path(p);
            r.ok = true;
            return r;
        }
    }
    return r;
}

std::string get_comspec() {
    char buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("ComSpec", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return "C:\\Windows\\System32\\cmd.exe";
    }
    return std::string(buf, n);
}

// Build a single command-line string for CreateProcess.
// Arguments with spaces or special characters are quoted.
std::string build_command_line(const std::string& command,
                                const std::vector<std::string>& args) {
    std::string cmdline;

    auto quote_arg = [](const std::string& arg) -> std::string {
        // If the arg contains spaces, tabs, or quotes, wrap in quotes
        bool needs_quoting = arg.empty() ||
            arg.find_first_of(" \t\"") != std::string::npos;
        if (!needs_quoting) return arg;

        std::string quoted = "\"";
        for (size_t i = 0; i < arg.size(); ++i) {
            if (arg[i] == '"') {
                quoted += "\\\"";
            } else if (arg[i] == '\\' && i + 1 < arg.size() && arg[i + 1] == '"') {
                quoted += "\\\\\\\"";
                ++i;
            } else {
                quoted += arg[i];
            }
        }
        quoted += '"';
        return quoted;
    };

    cmdline = quote_arg(command);
    for (const auto& a : args) {
        cmdline += ' ';
        cmdline += quote_arg(a);
    }
    return cmdline;
}

// Build a NUL-delimited environment block for CreateProcess
std::string build_env_block(const std::map<std::string, std::string>& overrides) {
    // Get current environment
    // GetEnvironmentStringsA returns a block of "KEY=VALUE\0KEY=VALUE\0\0"
    char* env = GetEnvironmentStringsA();
    if (!env) return "";

    std::map<std::string, std::string> env_map;
    const char* p = env;
    while (*p) {
        std::string entry(p);
        size_t eq = entry.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = entry.substr(0, eq);
            std::string val = entry.substr(eq + 1);
            // Skip if overridden
            if (overrides.find(key) == overrides.end()) {
                env_map[key] = val;
            }
        }
        p += entry.size() + 1;
    }
    FreeEnvironmentStringsA(env);

    // Apply overrides
    for (const auto& kv : overrides) {
        env_map[kv.first] = kv.second;
    }

    // Build the block
    std::string block;
    for (const auto& kv : env_map) {
        block += kv.first + "=" + kv.second;
        block.push_back('\0');
    }
    block.push_back('\0'); // Double NUL terminator
    return block;
}

// Read all available data from a pipe handle into a string.
// Non-blocking: returns immediately if no data available.
void drain_pipe(HANDLE pipe, std::string& output) {
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            break;
        }
        DWORD bytesRead = 0;
        DWORD toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
        if (!ReadFile(pipe, buf, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        output.append(buf, bytesRead);
    }
}

// Read remaining data from a pipe until it's closed
void read_pipe_to_end(HANDLE pipe, std::string& output) {
    char buf[4096];
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        output.append(buf, bytesRead);
    }
}

} // anonymous namespace

bool Win32ProcessRunner::is_sensitive_env_var(const std::string& name) {
    return ends_with(name, "_API_KEY") ||
           ends_with(name, "_SECRET") ||
           ends_with(name, "_TOKEN");
}

Result<ProcessResult> Win32ProcessRunner::run(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    int timeout_ms,
    const std::map<std::string, std::string>& env_overrides,
    const std::string& stdin_data)
{
    // Create pipes for stdout and stderr
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hStdoutRead = nullptr, hStdoutWrite = nullptr;
    HANDLE hStderrRead = nullptr, hStderrWrite = nullptr;
    HANDLE hStdinRead = nullptr, hStdinWrite = nullptr;

    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        return Result<ProcessResult>::failure("failed to create stdout pipe");
    }
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0)) {
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        return Result<ProcessResult>::failure("failed to create stderr pipe");
    }
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    if (!stdin_data.empty()) {
        if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
            CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
            CloseHandle(hStderrRead); CloseHandle(hStderrWrite);
            return Result<ProcessResult>::failure("failed to create stdin pipe");
        }
        SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    }

    // Create a Job Object to manage the process tree (like a process group)
    HANDLE hJob = CreateJobObjectA(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                &jobInfo, sizeof(jobInfo));
    }

    // Resolve the command against PATH + PATHEXT. Windows' CreateProcess only
    // appends ".exe", so a bare name like "codex" or "gemini" that is actually
    // installed as codex.cmd / gemini.cmd fails with ERROR_FILE_NOT_FOUND (2).
    ResolvedExe resolved = resolve_executable(command);
    if (!resolved.ok) {
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        CloseHandle(hStderrRead); CloseHandle(hStderrWrite);
        if (hStdinRead) CloseHandle(hStdinRead);
        if (hStdinWrite) CloseHandle(hStdinWrite);
        if (hJob) CloseHandle(hJob);
        return Result<ProcessResult>::failure(
            "could not find executable on PATH: " + command);
    }

    // Build the command line. For batch files, wrap in `cmd.exe /d /s /c "..."`
    // because CreateProcess cannot launch .cmd / .bat directly.
    std::string app_name;
    std::string cmdline;
    if (resolved.is_batch) {
        app_name = get_comspec();
        std::string inner = build_command_line(resolved.path, args);
        // /d: skip AutoRun. /s: cmd strips exactly the outer quote pair, which
        // lets the inner command line round-trip verbatim.
        cmdline = build_command_line(app_name, {"/d", "/s", "/c"}) + " \"" + inner + "\"";
    } else {
        app_name = resolved.path;
        cmdline = build_command_line(resolved.path, args);
    }

    // Build environment block
    std::string env_block;
    if (!env_overrides.empty()) {
        env_block = build_env_block(env_overrides);
    }

    // Set up startup info
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;
    si.hStdInput = stdin_data.empty() ? GetStdHandle(STD_INPUT_HANDLE) : hStdinRead;

    PROCESS_INFORMATION pi = {};

    // CreateProcess needs a mutable command line buffer
    std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back('\0');

    BOOL created = CreateProcessA(
        app_name.c_str(),       // lpApplicationName (resolved absolute path)
        cmdline_buf.data(),     // lpCommandLine
        nullptr,                // lpProcessAttributes
        nullptr,                // lpThreadAttributes
        TRUE,                   // bInheritHandles
        CREATE_NO_WINDOW | CREATE_SUSPENDED,  // dwCreationFlags
        env_block.empty() ? nullptr : const_cast<LPSTR>(env_block.c_str()), // lpEnvironment
        working_dir.empty() ? nullptr : working_dir.c_str(),  // lpCurrentDirectory
        &si,                    // lpStartupInfo
        &pi                     // lpProcessInformation
    );

    // Close child-side pipe handles in parent
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);
    if (hStdinRead) CloseHandle(hStdinRead);

    if (!created) {
        DWORD err = GetLastError();
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        if (hStdinWrite) CloseHandle(hStdinWrite);
        if (hJob) CloseHandle(hJob);
        return Result<ProcessResult>::failure(
            "CreateProcess failed (error " + std::to_string(err) + ") for: " + command);
    }

    // Assign process to job object
    if (hJob) {
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    // Resume the process now that it's in the job
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Track active process for cancellation
    {
        std::lock_guard<std::mutex> lock(procs_mutex_);
        active_procs_.push_back({pi.hProcess, hJob});
    }

    // Write stdin data
    if (hStdinWrite) {
        const char* ptr = stdin_data.data();
        DWORD remaining = static_cast<DWORD>(stdin_data.size());
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(hStdinWrite, ptr, remaining, &written, nullptr)) break;
            ptr += written;
            remaining -= written;
        }
        CloseHandle(hStdinWrite);
        hStdinWrite = nullptr;
    }

    // Poll for output with timeout
    std::string stdout_data;
    std::string stderr_data;
    bool timed_out = false;

    DWORD deadline = timeout_ms > 0
        ? GetTickCount() + static_cast<DWORD>(timeout_ms)
        : 0;

    for (;;) {
        drain_pipe(hStdoutRead, stdout_data);
        drain_pipe(hStderrRead, stderr_data);

        // Check if process has exited
        DWORD wait_result = WaitForSingleObject(pi.hProcess, 100);
        if (wait_result == WAIT_OBJECT_0) {
            // Process exited — drain remaining output
            read_pipe_to_end(hStdoutRead, stdout_data);
            read_pipe_to_end(hStderrRead, stderr_data);
            break;
        }

        // Check timeout
        if (deadline > 0 && GetTickCount() >= deadline) {
            timed_out = true;
            // Terminate via job object (kills entire process tree)
            if (hJob) {
                TerminateJobObject(hJob, 1);
            } else {
                TerminateProcess(pi.hProcess, 1);
            }
            // Drain remaining output
            drain_pipe(hStdoutRead, stdout_data);
            drain_pipe(hStderrRead, stderr_data);
            break;
        }
    }

    // Get exit code
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    // Cleanup
    CloseHandle(hStdoutRead);
    CloseHandle(hStderrRead);

    // Remove from active list
    {
        std::lock_guard<std::mutex> lock(procs_mutex_);
        active_procs_.erase(
            std::remove_if(active_procs_.begin(), active_procs_.end(),
                [&](const ActiveProcess& ap) { return ap.process == pi.hProcess; }),
            active_procs_.end());
    }

    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);

    ProcessResult result;
    result.timed_out = timed_out;
    result.stdout_output = std::move(stdout_data);
    result.stderr_output = std::move(stderr_data);
    result.exit_code = static_cast<int>(exit_code);

    return Result<ProcessResult>::success(std::move(result));
}

void Win32ProcessRunner::kill_all() {
    std::lock_guard<std::mutex> lock(procs_mutex_);
    for (auto& ap : active_procs_) {
        if (ap.job) {
            TerminateJobObject(ap.job, 1);
        } else {
            TerminateProcess(ap.process, 1);
        }
    }
}

} // namespace needle

#endif // _WIN32
