#include <catch2/catch.hpp>

#include "needle/troubleshoot/session_id.h"

#include <regex>
#include <set>

using namespace needle;

TEST_CASE("Troubleshoot session id uses timestamp plus random suffix",
          "[troubleshoot][session_id]") {
    const std::regex pattern(R"(^[0-9-]+T[0-9-]+Z-[0-9a-f]{4}$)");
    std::set<std::string> ids;

    for (int i = 0; i < 100; ++i) {
        std::string id = make_troubleshoot_session_id();
        REQUIRE(std::regex_match(id, pattern));
        ids.insert(id);
    }

    REQUIRE(ids.size() == 100);
}

TEST_CASE("Troubleshoot session id generation stays bounded-format past dedup cap",
          "[troubleshoot][session_id]") {
    const std::regex pattern(R"(^[0-9-]+T[0-9-]+Z-[0-9a-f]{4}$)");

    for (int i = 0; i < 5000; ++i) {
        std::string id = make_troubleshoot_session_id();
        REQUIRE(std::regex_match(id, pattern));
    }
}
