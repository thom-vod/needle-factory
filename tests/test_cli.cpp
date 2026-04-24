#include <catch2/catch.hpp>
#include <atomic>

// We test the Router's argument parsing by including the header
// and using dispatch with various argument combinations
#include "../src/cli/router.h"

using namespace needle;

namespace {

// Helper to create argc/argv from string vectors
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

} // anonymous namespace

TEST_CASE("CLI: --help returns 0", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "--help"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 0);
}

TEST_CASE("CLI: --version returns 0", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "--version"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 0);
}

TEST_CASE("CLI: no arguments returns 2 (usage error)", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 2);
}

TEST_CASE("CLI: unknown command returns 2", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "unknown_cmd"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 2);
}

TEST_CASE("CLI: run without file returns 2", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "run"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 2);
}

TEST_CASE("CLI: validate without file returns 2", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "validate"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 2);
}

TEST_CASE("CLI: run with nonexistent file returns 1", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "run", "/nonexistent/file.dot"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 1);
}

TEST_CASE("CLI: validate with nonexistent file returns 1", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "validate", "/nonexistent/file.dot"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 1);
}

TEST_CASE("CLI: -h is alias for --help", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "-h"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 0);
}

TEST_CASE("CLI: -v is alias for --version", "[cli]") {
    std::atomic<bool> cancelled(false);
    Router router(cancelled);

    ArgHelper args({"needle", "-v"});
    int rc = router.dispatch(args.argc(), args.argv());
    REQUIRE(rc == 0);
}
