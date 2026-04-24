#include <catch2/catch.hpp>
#include "needle/util/resource_locator.h"
#include "needle/util/fs_helpers.h"

#include <fstream>
#include <cstdlib>
#include <unistd.h>

using namespace needle;

TEST_CASE("ResourceLocator find returns failure for nonexistent file", "[resource_locator]") {
    ResourceLocator locator;
    auto result = locator.find("scripts", "nonexistent_12345.sh");
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().find("tried:") != std::string::npos);
}

TEST_CASE("ResourceLocator find succeeds when file exists in search path", "[resource_locator]") {
    // Use NEEDLE_SOURCE_DIR as the exe_dir parent, so prefix resolves to the source tree
    std::string src_dir = NEEDLE_SOURCE_DIR;
    // The exe is in build/, so exe_dir = <src>/build => prefix = <src>
    std::string fake_exe_dir = src_dir + "/build";
    ResourceLocator locator(fake_exe_dir);

    // sample_dots/simple_pipeline.dot should exist under prefix
    auto result = locator.find("sample_dots", "simple_pipeline.dot");
    REQUIRE(result.ok());
    // The returned path should contain the filename
    REQUIRE(result.value().find("simple_pipeline.dot") != std::string::npos);
}

TEST_CASE("ResourceLocator find_dir locates sample_dots", "[resource_locator]") {
    std::string src_dir = NEEDLE_SOURCE_DIR;
    std::string fake_exe_dir = src_dir + "/build";
    ResourceLocator locator(fake_exe_dir);

    auto result = locator.find_dir("sample_dots");
    REQUIRE(result.ok());
    REQUIRE(result.value().find("sample_dots") != std::string::npos);
}

TEST_CASE("ResourceLocator custom exe_dir is used correctly", "[resource_locator]") {
    ResourceLocator locator("/some/custom/path");
    REQUIRE(locator.exe_dir() == "/some/custom/path");
    // Search roots should include /some/custom as prefix-based roots
    const auto& roots = locator.search_roots();
    REQUIRE(roots.size() == 3);
}

TEST_CASE("ResourceLocator find_executable returns a valid path", "[resource_locator]") {
    ResourceLocator locator;
    auto result = locator.find_executable();
    // In a test binary, this should succeed on macOS/Linux
    REQUIRE(result.ok());
    REQUIRE_FALSE(result.value().empty());
    // The path should point to an actual file
    REQUIRE(needle::is_file(result.value()));
}

TEST_CASE("ResourceLocator find_dir returns failure for nonexistent dir", "[resource_locator]") {
    ResourceLocator locator;
    auto result = locator.find_dir("nonexistent_dir_12345");
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error().find("tried:") != std::string::npos);
}

TEST_CASE("fs_helpers mkdir_p creates nested directories", "[fs_helpers]") {
    std::string base = "/tmp/needle_test_mkdir_p_" + std::to_string(getpid());
    std::string deep = base + "/a/b/c";

    REQUIRE(needle::mkdir_p(deep));
    REQUIRE(needle::is_directory(deep));
    REQUIRE(needle::path_exists(deep));
    REQUIRE_FALSE(needle::is_file(deep));

    // Cleanup
    rmdir((base + "/a/b/c").c_str());
    rmdir((base + "/a/b").c_str());
    rmdir((base + "/a").c_str());
    rmdir(base.c_str());
}

TEST_CASE("fs_helpers path_exists and is_file", "[fs_helpers]") {
    std::string path = "/tmp/needle_test_fs_" + std::to_string(getpid());
    {
        std::ofstream out(path);
        out << "test";
    }
    REQUIRE(needle::path_exists(path));
    REQUIRE(needle::is_file(path));
    REQUIRE_FALSE(needle::is_directory(path));

    unlink(path.c_str());
    REQUIRE_FALSE(needle::path_exists(path));
}
