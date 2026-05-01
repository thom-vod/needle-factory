#include <catch2/catch.hpp>
#include "needle/backend/process_runner.h"
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace needle;

TEST_CASE("MockProcessRunner: canned responses work", "[process_runner]") {
    MockProcessRunner mock;

    ProcessResult resp;
    resp.exit_code = 0;
    resp.stdout_output = "hello world";
    resp.stderr_output = "";
    resp.timed_out = false;
    mock.enqueue(resp);

    auto result = mock.run("echo", {"hello", "world"}, ".", 5000);
    REQUIRE(result.ok());
    REQUIRE(result.value().exit_code == 0);
    REQUIRE(result.value().stdout_output == "hello world");

    auto calls = mock.calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].command == "echo");
    REQUIRE(calls[0].args.size() == 2);
}

TEST_CASE("MockProcessRunner: empty queue returns failure", "[process_runner]") {
    MockProcessRunner mock;
    auto result = mock.run("test", {}, ".", 1000);
    REQUIRE_FALSE(result.ok());
}

TEST_CASE("NativeProcessRunner: run echo captures stdout", "[process_runner]") {
    NativeProcessRunner runner;
#ifdef _WIN32
    auto result = runner.run("cmd.exe", {"/C", "echo", "hello"}, ".", 5000);
#else
    auto result = runner.run("/bin/echo", {"hello"}, ".", 5000);
#endif
    REQUIRE(result.ok());
    REQUIRE(result.value().exit_code == 0);
    // Output includes trailing newline; Windows cmd echo may include \r\n
    REQUIRE(result.value().stdout_output.find("hello") != std::string::npos);
    REQUIRE_FALSE(result.value().timed_out);
}

TEST_CASE("NativeProcessRunner: run false returns non-zero exit code", "[process_runner]") {
    NativeProcessRunner runner;
#ifdef _WIN32
    auto result = runner.run("cmd.exe", {"/C", "exit", "1"}, ".", 5000);
#else
    auto result = runner.run("/usr/bin/false", {}, ".", 5000);
#endif
    REQUIRE(result.ok());
    REQUIRE(result.value().exit_code != 0);
    REQUIRE_FALSE(result.value().timed_out);
}

TEST_CASE("NativeProcessRunner: timeout test", "[process_runner]") {
    NativeProcessRunner runner;
#ifdef _WIN32
    // ping -n 10 localhost takes ~10 seconds; timeout after 500ms
    auto result = runner.run("ping", {"-n", "10", "127.0.0.1"}, ".", 500);
#else
    // Sleep for 10 seconds but timeout after 500ms
    auto result = runner.run("/bin/sleep", {"10"}, ".", 500);
#endif
    REQUIRE(result.ok());
    REQUIRE(result.value().timed_out);
}

#ifdef _WIN32
TEST_CASE("Win32ProcessRunner: bare name resolves .cmd via PATHEXT",
          "[process_runner]") {
    // CreateProcess itself does not walk PATHEXT — it only appends ".exe". This
    // verifies the runner's own resolution step finds a bare `.cmd` (which is
    // how npm-installed CLIs like gemini-cli and the codex npm shim land on
    // Windows).

    char temp_buf[MAX_PATH];
    DWORD tn = GetTempPathA(MAX_PATH, temp_buf);
    REQUIRE(tn != 0);
    std::string scratch = std::string(temp_buf, tn) + "needle-pathext-test";
    CreateDirectoryA(scratch.c_str(), nullptr);

    std::string cmd_path = scratch + "\\nlpathexttest.cmd";
    {
        FILE* f = std::fopen(cmd_path.c_str(), "w");
        REQUIRE(f != nullptr);
        std::fputs("@echo off\r\necho resolved-ok\r\n", f);
        std::fclose(f);
    }

    // Prepend scratch to the current process PATH so SearchPathA finds it.
    std::string old_path;
    {
        DWORD need = GetEnvironmentVariableA("PATH", nullptr, 0);
        std::vector<char> buf(need ? need : 1);
        DWORD got = GetEnvironmentVariableA("PATH", buf.data(),
                                            static_cast<DWORD>(buf.size()));
        old_path.assign(buf.data(), got);
    }
    std::string new_path = scratch + ";" + old_path;
    SetEnvironmentVariableA("PATH", new_path.c_str());

    NativeProcessRunner runner;
    auto result = runner.run("nlpathexttest", {}, ".", 10000);

    // Restore PATH and clean up before assertions so a failure doesn't leak.
    SetEnvironmentVariableA("PATH", old_path.c_str());
    std::remove(cmd_path.c_str());
    RemoveDirectoryA(scratch.c_str());

    REQUIRE(result.ok());
    CHECK(result.value().exit_code == 0);
    CHECK_FALSE(result.value().timed_out);
    CHECK(result.value().stdout_output.find("resolved-ok") != std::string::npos);
}

TEST_CASE("Win32ProcessRunner: missing command yields a clear error",
          "[process_runner]") {
    NativeProcessRunner runner;
    auto result = runner.run("nlpathext_does_not_exist_xyzzy", {}, ".", 5000);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("could not find executable") != std::string::npos);
}
#endif

#ifndef _WIN32
TEST_CASE("NativeProcessRunner: returns promptly when direct child exits "
          "even if a backgrounded grandchild inherits stdio",
          "[process_runner]") {
    // Shell that spawns a backgrounded child (inheriting the shell's stdout)
    // and then exits immediately. Before the WNOHANG reap check, the runner
    // waited for the backgrounded `sleep 30` to close the pipes — timing
    // out 30 seconds later. With the fix, the runner detects the direct
    // child exit and returns within a small multiple of the poll interval
    // (500 ms).
    NativeProcessRunner runner;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    auto result = runner.run(
        "/bin/sh",
        {"-c", "sleep 30 & echo done"},
        ".",
        /*timeout_ms=*/15000);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000;

    REQUIRE(result.ok());
    REQUIRE(result.value().exit_code == 0);
    REQUIRE_FALSE(result.value().timed_out);
    REQUIRE(result.value().stdout_output.find("done") != std::string::npos);
    // Should return well under the timeout — pre-fix this took 15s (timeout)
    // or 30s (waited for sleep). Allow generous slack but ensure we're far
    // below the timeout.
    REQUIRE(elapsed_ms < 5000);
}

TEST_CASE("NativeProcessRunner: idle_timeout kills a silent process",
          "[process_runner][idle_timeout]") {
    // A process that produces no output for `idle_timeout_ms` should be killed
    // with timeout_kind=Idle, even if total elapsed time is under the wall
    // clock cap. This is the headline N1 mechanic — fast-fail on stalls.
    NativeProcessRunner runner;
    std::map<std::string, std::string> env;
    auto result = runner.run("/bin/sh", {"-c", "sleep 5"}, ".",
                             /*timeout_ms=*/30000,
                             env, /*stdin_data=*/"",
                             /*idle_timeout_ms=*/500);
    REQUIRE(result.ok());
    REQUIRE(result.value().timed_out);
    REQUIRE(result.value().timeout_kind == TimeoutKind::Idle);
    REQUIRE_FALSE(result.value().timeout_kind == TimeoutKind::WallClock);
}

TEST_CASE("NativeProcessRunner: idle_timeout resets on output",
          "[process_runner][idle_timeout]") {
    // Continuous heartbeat output should reset the idle timer indefinitely.
    // The process below prints a line every 50ms for ~2s; 200ms idle threshold
    // would erroneously fire if reset weren't working.
    NativeProcessRunner runner;
    std::map<std::string, std::string> env;
    auto result = runner.run(
        "/bin/sh",
        {"-c", "i=0; while [ $i -lt 20 ]; do echo .; sleep 0.05; i=$((i+1)); done"},
        ".",
        /*timeout_ms=*/10000,
        env, /*stdin_data=*/"",
        /*idle_timeout_ms=*/500);
    REQUIRE(result.ok());
    REQUIRE_FALSE(result.value().timed_out);
    REQUIRE(result.value().exit_code == 0);
    // Should have ~20 output lines.
    REQUIRE(result.value().stdout_output.size() >= 20);
}

TEST_CASE("NativeProcessRunner: idle_timeout=0 disables idle tracking",
          "[process_runner][idle_timeout]") {
    // 0 means disabled — silent process under wall-clock budget should
    // succeed normally.
    NativeProcessRunner runner;
    std::map<std::string, std::string> env;
    auto result = runner.run("/bin/sh", {"-c", "sleep 0.3"}, ".",
                             /*timeout_ms=*/5000,
                             env, /*stdin_data=*/"",
                             /*idle_timeout_ms=*/0);
    REQUIRE(result.ok());
    REQUIRE_FALSE(result.value().timed_out);
    REQUIRE(result.value().timeout_kind == TimeoutKind::None);
    REQUIRE(result.value().exit_code == 0);
}

TEST_CASE("NativeProcessRunner: wall-clock takes precedence over idle when "
          "both would fire",
          "[process_runner][idle_timeout]") {
    // Wall-clock 200ms vs idle 1000ms — wall-clock fires first on a silent
    // process. timeout_kind reflects which limit hit.
    NativeProcessRunner runner;
    std::map<std::string, std::string> env;
    auto result = runner.run("/bin/sh", {"-c", "sleep 5"}, ".",
                             /*timeout_ms=*/200,
                             env, /*stdin_data=*/"",
                             /*idle_timeout_ms=*/1000);
    REQUIRE(result.ok());
    REQUIRE(result.value().timed_out);
    REQUIRE(result.value().timeout_kind == TimeoutKind::WallClock);
}
#endif
