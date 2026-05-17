#include <catch2/catch.hpp>

#include "needle/troubleshoot/types.h"
#include "needle/util/logger.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

using namespace needle;

namespace {

int count_occurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    std::string::size_type pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

TEST_CASE("legacy troubleshoot_on_failure booleans parse to tier modes", "[troubleshoot][compat]") {
    Maybe<TroubleshootMode> true_mode = parse_troubleshoot_mode("true");
    REQUIRE(true_mode.has_value());
    REQUIRE(*true_mode == TroubleshootMode::Tweak);

    Maybe<TroubleshootMode> false_mode = parse_troubleshoot_mode("false");
    REQUIRE(false_mode.has_value());
    REQUIRE(*false_mode == TroubleshootMode::Off);
}

TEST_CASE("legacy troubleshoot_on_failure deprecation warning is emitted once per literal", "[troubleshoot][compat]") {
    std::string path = "/tmp/needle_test_troubleshoot_compat_" + std::to_string(getpid()) + ".log";
    Logger& logger = global_logger();
    LogLevel old_level = logger.level();
    logger.set_level(LogLevel::WARN);
    REQUIRE(logger.set_file(path));

    REQUIRE(parse_troubleshoot_mode_graph_attr("true").has_value());
    REQUIRE(parse_troubleshoot_mode_graph_attr("true").has_value());
    REQUIRE(parse_troubleshoot_mode_graph_attr("false").has_value());
    REQUIRE(parse_troubleshoot_mode_graph_attr("false").has_value());

    logger.close_file();
    logger.set_level(old_level);

    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    REQUIRE(count_occurrences(content, "troubleshoot_on_failure=\"true\" is deprecated") == 1);
    REQUIRE(count_occurrences(content, "troubleshoot_on_failure=\"false\" is deprecated") == 1);
    REQUIRE(content.find("use troubleshoot_on_failure=\"tweak\" instead") != std::string::npos);
    REQUIRE(content.find("use troubleshoot_on_failure=\"off\" instead") != std::string::npos);

    std::remove(path.c_str());
}
