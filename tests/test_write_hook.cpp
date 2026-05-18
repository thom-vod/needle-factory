#include <catch2/catch.hpp>

#include "needle/platform/platform.h"
#include "needle/troubleshoot/write_hook.h"

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace needle;

namespace {

struct Scratch {
    std::string root;
    std::string project_dir;
    std::string session_dir;

    Scratch() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        root = platform::path_join(platform::temp_dir(),
                                   "needle_write_hook_" + std::to_string(unique));
        project_dir = platform::path_join(root, "project");
        session_dir = platform::path_join(root, "session");
        platform::remove_recursive(root);
        platform::mkdir_p(project_dir + "/src");
        platform::mkdir_p(session_dir);
    }

    ~Scratch() {
        platform::remove_recursive(root);
    }
};

void write_file(const std::string& path, const std::string& value) {
    std::ofstream out(path);
    out << value;
}

nlohmann::json assistant_tool_use(const std::string& id,
                                  const std::string& name,
                                  const std::string& file_path) {
    nlohmann::json block;
    block["type"] = "tool_use";
    block["id"] = id;
    block["name"] = name;
    block["input"]["file_path"] = file_path;

    nlohmann::json line;
    line["type"] = "assistant";
    line["message"]["content"] = nlohmann::json::array({block});
    return line;
}

} // namespace

TEST_CASE("file_write_allowed accepts paths inside project_dir", "[write_hook]") {
    Scratch scratch;
    REQUIRE(file_write_allowed(scratch.project_dir + "/src/new_file.txt",
                               scratch.project_dir,
                               scratch.session_dir));
}

TEST_CASE("file_write_allowed accepts paths inside session_dir", "[write_hook]") {
    Scratch scratch;
    REQUIRE(file_write_allowed(scratch.session_dir + "/recovery.md",
                               scratch.project_dir,
                               scratch.session_dir));
}

TEST_CASE("file_write_allowed rejects paths outside both", "[write_hook]") {
    Scratch scratch;
    REQUIRE_FALSE(file_write_allowed(scratch.root + "/outside.txt",
                                     scratch.project_dir,
                                     scratch.session_dir));
}

TEST_CASE("file_write_allowed rejects literal tilde segments (m-c)", "[write_hook]") {
    Scratch scratch;
    // Unexpanded home-dir references must not slip through the audit, since
    // the on-disk write may land under $HOME even though canonicalisation
    // erases the segment.
    REQUIRE_FALSE(file_write_allowed("~/secret", scratch.project_dir, scratch.session_dir));
    REQUIRE_FALSE(file_write_allowed("~", scratch.project_dir, scratch.session_dir));
    REQUIRE_FALSE(file_write_allowed("/some/path/~/inside",
                                     scratch.project_dir, scratch.session_dir));
    REQUIRE_FALSE(file_write_allowed("/some/path/~",
                                     scratch.project_dir, scratch.session_dir));
    // Literal `~tilde-prefixed-filename` is NOT a home-dir reference; allow.
    REQUIRE(file_write_allowed(scratch.project_dir + "/~tilde-name.txt",
                               scratch.project_dir, scratch.session_dir));
}

TEST_CASE("audit_events_ndjson tolerates flat j.content shape (m-b)", "[write_hook]") {
    Scratch scratch;
    const std::string events_path = scratch.session_dir + "/events.ndjson";
    nlohmann::json block;
    block["type"] = "tool_use";
    block["id"] = "toolu_flat";
    block["name"] = "Write";
    block["input"]["file_path"] = "/etc/passwd";

    nlohmann::json line;
    line["type"] = "assistant";
    line["content"] = nlohmann::json::array({block}); // flat shape (no `message` wrapper)

    write_file(events_path, line.dump() + "\n");
    auto violations = audit_events_ndjson(events_path, scratch.project_dir, scratch.session_dir);
    REQUIRE(violations.size() == 1);
    REQUIRE(violations[0].tool_use_id == "toolu_flat");
}

TEST_CASE("audit_events_ndjson returns empty for clean stream", "[write_hook]") {
    Scratch scratch;
    const std::string events_path = scratch.session_dir + "/events.ndjson";
    std::ostringstream body;
    body << R"({"type":"assistant","message":{"content":[{"type":"text","text":"hello"}]}})" << "\n";
    body << assistant_tool_use("toolu_read", "Read", "/etc/passwd").dump() << "\n";
    write_file(events_path, body.str());

    auto violations = audit_events_ndjson(events_path, scratch.project_dir, scratch.session_dir);
    REQUIRE(violations.empty());
}

TEST_CASE("audit_events_ndjson catches Write outside project_dir", "[write_hook]") {
    Scratch scratch;
    const std::string events_path = scratch.session_dir + "/events.ndjson";
    std::ostringstream body;
    body << assistant_tool_use("toolu_write", "Write", "/etc/passwd").dump() << "\n";
    write_file(events_path, body.str());

    auto violations = audit_events_ndjson(events_path, scratch.project_dir, scratch.session_dir);
    REQUIRE(violations.size() == 1);
    REQUIRE(violations[0].tool == "Write");
    REQUIRE(violations[0].file_path == "/etc/passwd");
    REQUIRE(violations[0].tool_use_id == "toolu_write");
}
