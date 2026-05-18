#include <catch2/catch.hpp>

#include "needle/platform/platform.h"
#include "needle/troubleshoot/allowed_tools.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace needle;

namespace {

constexpr const char* kClaudePath = "/opt/homebrew/bin/claude";

struct scratch_dir {
    std::string path;

    explicit scratch_dir(std::string p) : path(std::move(p)) {}

    ~scratch_dir() {
        if (!path.empty()) {
            platform::remove_recursive(path);
        }
    }
};

struct command_result {
    std::string output;
    int status = -1;
};

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

int exit_code(int status) {
#ifndef _WIN32
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
#endif
    return status;
}

command_result run_capture(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    REQUIRE(pipe != nullptr);

    command_result result;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.output += buffer;
    }

#ifdef _WIN32
    result.status = _pclose(pipe);
#else
    result.status = pclose(pipe);
#endif
    return result;
}

void run_system_in_dir(const std::string& cwd, const std::string& command) {
    int status = std::system(("cd " + shell_quote(cwd) + " && " + command).c_str());
    INFO(command);
    REQUIRE(exit_code(status) == 0);
}

void write_file(const std::string& path, const std::string& contents) {
    std::ofstream out(path);
    REQUIRE(out.good());
    out << contents;
    REQUIRE(out.good());
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

std::string compact_json_line(const std::string& line) {
    std::string compact;
    compact.reserve(line.size());
    for (unsigned char ch : line) {
        if (!std::isspace(ch)) {
            compact += static_cast<char>(ch);
        }
    }
    return compact;
}

bool is_json_line(const std::string& line) {
    auto it = std::find_if_not(line.begin(), line.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    return it != line.end() && *it == '{';
}

void require_no_permission_errors(const std::string& output) {
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!is_json_line(line)) {
            continue;
        }

        std::string compact = compact_json_line(line);
        INFO(line);
        CHECK(compact.find("\"subtype\":\"error_permission_denied\"") == std::string::npos);
        CHECK(compact.find("\"is_error\":true") == std::string::npos);
    }
}

scratch_dir make_scratch_dir() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string path = platform::path_join(platform::temp_dir(),
                                           "needle-allowed-tools-smoke-" + std::to_string(stamp));
    REQUIRE(platform::mkdir_p(path));
    return scratch_dir(path);
}

bool live_claude_enabled() {
    const char* value = std::getenv("NEEDLE_LIVE_CLAUDE");
    return value != nullptr && std::string(value) == "1";
}

void run_allowed_tools_smoke(TroubleshootMode mode, const std::string& task, bool expect_edit) {
    if (!live_claude_enabled()) {
        SUCCEED("SKIP: set NEEDLE_LIVE_CLAUDE=1 to run live claude allow-list smoke tests");
        return;
    }

    scratch_dir scratch = make_scratch_dir();
    run_system_in_dir(scratch.path, "git init -q");
    write_file(platform::path_join(scratch.path, "graph.dot"), "digraph g {\n  a -> b;\n}\n");
    run_system_in_dir(scratch.path, "git add graph.dot");
    run_system_in_dir(scratch.path,
                      "git -c user.name=needle-smoke "
                      "-c user.email=needle-smoke@example.invalid commit -q -m init");

    std::string stage_dir = platform::path_join(scratch.path, ".needle/run-x/stages/a");
    REQUIRE(platform::mkdir_p(stage_dir));
    write_file(platform::path_join(stage_dir, "prompt.md"), "hello\n");

    std::string allowed_tools = build_allowed_tools(mode,
                                                    scratch.path,
                                                    "graph.dot",
                                                    platform::path_join(scratch.path, "recovery"));
    std::string command = "cd " + shell_quote(scratch.path) + " && " + shell_quote(kClaudePath) +
                          " --output-format stream-json"
                          " --verbose"
                          " --permission-mode default"
                          " --allowed-tools " + shell_quote(allowed_tools) +
                          " -p " + shell_quote(task) +
                          " 2>&1";

    command_result result = run_capture(command);
    INFO(result.output);
    REQUIRE(exit_code(result.status) == 0);
    require_no_permission_errors(result.output);

    if (expect_edit) {
        std::string graph = read_file(platform::path_join(scratch.path, "graph.dot"));
        REQUIRE(graph.find("// smoke ok") != std::string::npos);
    }
}

} // namespace

TEST_CASE("allowed tools diagnose live claude smoke", "[allowed_tools_smoke][.live]") {
    run_allowed_tools_smoke(TroubleshootMode::Diagnose,
                            "Read graph.dot, briefly describe it, then exit.",
                            false);
}

TEST_CASE("allowed tools tweak live claude smoke", "[allowed_tools_smoke][.live]") {
    run_allowed_tools_smoke(TroubleshootMode::Tweak,
                            "Append the line // smoke ok to the end of graph.dot using Edit, then exit.",
                            true);
}

TEST_CASE("allowed tools full live claude smoke", "[allowed_tools_smoke][.live]") {
    run_allowed_tools_smoke(TroubleshootMode::Full,
                            "Append the line // smoke ok to the end of graph.dot using Edit, then exit.",
                            true);
}
