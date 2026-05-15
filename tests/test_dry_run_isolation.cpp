#include <catch2/catch.hpp>

#include "../src/cli/router.h"
#include "needle/platform/platform.h"
#include "support/temp_needle_state.h"

#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <sys/types.h>
#endif

using namespace needle;

namespace {

struct ArgHelper {
    std::vector<std::string> args_storage;
    std::vector<char*> argv_ptrs;

    ArgHelper(std::initializer_list<std::string> args) {
        args_storage.assign(args.begin(), args.end());
        for (auto& s : args_storage) {
            argv_ptrs.push_back(&s[0]);
        }
    }

    int argc() const { return static_cast<int>(argv_ptrs.size()); }
    char** argv() { return argv_ptrs.data(); }
};

struct FileSnapshot {
    std::string content;
    long long mtime_sec = 0;
    long mtime_nsec = 0;
};

void write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out << content;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

FileSnapshot snapshot_file(const std::string& path) {
    FileSnapshot snap;
    snap.content = read_file(path);

#ifdef _WIN32
    struct _stat st;
    REQUIRE(_stat(path.c_str(), &st) == 0);
    snap.mtime_sec = static_cast<long long>(st.st_mtime);
#else
    struct stat st;
    REQUIRE(stat(path.c_str(), &st) == 0);
    snap.mtime_sec = static_cast<long long>(st.st_mtime);
#if defined(__APPLE__)
    snap.mtime_nsec = st.st_mtimespec.tv_nsec;
#elif defined(st_mtim)
    snap.mtime_nsec = st.st_mtim.tv_nsec;
#endif
#endif

    return snap;
}

void require_unchanged(const std::string& path, const FileSnapshot& before) {
    FileSnapshot after = snapshot_file(path);
    REQUIRE(after.content == before.content);
    REQUIRE(after.mtime_sec == before.mtime_sec);
    REQUIRE(after.mtime_nsec == before.mtime_nsec);
}

} // namespace

TEST_CASE("CLI dry-run writes to isolated logs root without touching real run artifacts",
          "[cli][dry_run]") {
    tests::TempNeedleState state;
    std::string project_dir = state.path("project");
    std::string dot_path = platform::path_join(project_dir, "workflow.dot");
    std::string real_root = platform::path_join(project_dir, ".needle/workflow");
    std::string real_stage = platform::path_join(real_root, "stages/work");
    std::string dry_root = platform::path_join(project_dir, ".needle/workflow-dryrun");
    std::string dry_stage_status = platform::path_join(dry_root, "stages/work/status.json");

    REQUIRE(platform::mkdir_p(project_dir));
    REQUIRE(platform::mkdir_p(real_stage));

    write_file(dot_path,
        "digraph workflow {\n"
        "  start [shape=Mdiamond, label=\"Start\"];\n"
        "  work [shape=box, label=\"Work\", prompt=\"Do a dry-run check\"];\n"
        "  exit [shape=Msquare, label=\"Done\"];\n"
        "  start -> work;\n"
        "  work -> exit;\n"
        "}\n");
    write_file(platform::path_join(real_root, "checkpoint.json"),
               "{\"sentinel\":\"real checkpoint\"}\n");
    write_file(platform::path_join(real_stage, "status.json"),
               "{\"sentinel\":\"real stage status\"}\n");

    FileSnapshot checkpoint_before =
        snapshot_file(platform::path_join(real_root, "checkpoint.json"));
    FileSnapshot status_before =
        snapshot_file(platform::path_join(real_stage, "status.json"));

    std::atomic<bool> cancelled(false);
    Router router(cancelled);
    ArgHelper args({"needle", "run", "--dry-run", "--project-dir", project_dir, dot_path});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 0);

    require_unchanged(platform::path_join(real_root, "checkpoint.json"), checkpoint_before);
    require_unchanged(platform::path_join(real_stage, "status.json"), status_before);

    REQUIRE(platform::is_directory(dry_root));
    REQUIRE(platform::file_exists(platform::path_join(dry_root, "checkpoint.json")));
    REQUIRE(platform::file_exists(dry_stage_status));
    REQUIRE(read_file(dry_stage_status).find("[dry-run] codergen") != std::string::npos);
}
