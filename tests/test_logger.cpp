#include <catch2/catch.hpp>
#include "needle/util/logger.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <unistd.h>

using namespace needle;

TEST_CASE("log_level_name returns correct strings", "[logger]") {
    REQUIRE(std::string(log_level_name(LogLevel::TRACE)) == "TRACE");
    REQUIRE(std::string(log_level_name(LogLevel::DEBUG)) == "DEBUG");
    REQUIRE(std::string(log_level_name(LogLevel::INFO)) == "INFO");
    REQUIRE(std::string(log_level_name(LogLevel::WARN)) == "WARN");
    REQUIRE(std::string(log_level_name(LogLevel::ERROR)) == "ERROR");
}

TEST_CASE("Logger default level is INFO", "[logger]") {
    Logger logger;
    REQUIRE(logger.level() == LogLevel::INFO);
}

TEST_CASE("Logger level filtering", "[logger]") {
    Logger logger;

    // Default level is INFO
    REQUIRE(logger.is_enabled(LogLevel::INFO));
    REQUIRE(logger.is_enabled(LogLevel::WARN));
    REQUIRE(logger.is_enabled(LogLevel::ERROR));
    REQUIRE_FALSE(logger.is_enabled(LogLevel::DEBUG));
    REQUIRE_FALSE(logger.is_enabled(LogLevel::TRACE));

    // Set to DEBUG
    logger.set_level(LogLevel::DEBUG);
    REQUIRE(logger.is_enabled(LogLevel::DEBUG));
    REQUIRE(logger.is_enabled(LogLevel::INFO));
    REQUIRE_FALSE(logger.is_enabled(LogLevel::TRACE));

    // Set to ERROR
    logger.set_level(LogLevel::ERROR);
    REQUIRE(logger.is_enabled(LogLevel::ERROR));
    REQUIRE_FALSE(logger.is_enabled(LogLevel::WARN));
    REQUIRE_FALSE(logger.is_enabled(LogLevel::INFO));
}

TEST_CASE("Logger file logging writes to file", "[logger]") {
    std::string path = "/tmp/needle_test_logger_" + std::to_string(getpid()) + ".log";

    Logger logger;
    logger.set_level(LogLevel::TRACE);
    REQUIRE(logger.set_file(path));

    logger.log(LogLevel::INFO, "test", "hello %s %d", "world", 42);
    logger.log(LogLevel::DEBUG, "cat2", "debug message");
    logger.close_file();

    // Read back and verify
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    // Should contain both messages with timestamps and categories
    REQUIRE(content.find("[INFO] [test] hello world 42") != std::string::npos);
    REQUIRE(content.find("[DEBUG] [cat2] debug message") != std::string::npos);
    // Should contain ISO 8601 timestamp (starts with 20xx-)
    REQUIRE(content.find("20") != std::string::npos);
    REQUIRE(content.find("T") != std::string::npos);
    REQUIRE(content.find("Z") != std::string::npos);

    std::remove(path.c_str());
}

TEST_CASE("Logger file logging respects level filter", "[logger]") {
    std::string path = "/tmp/needle_test_logger_filter_" + std::to_string(getpid()) + ".log";

    Logger logger;
    logger.set_level(LogLevel::WARN);
    REQUIRE(logger.set_file(path));

    logger.log(LogLevel::DEBUG, "test", "should not appear");
    logger.log(LogLevel::INFO, "test", "should not appear either");
    logger.log(LogLevel::WARN, "test", "warning message");
    logger.log(LogLevel::ERROR, "test", "error message");
    logger.close_file();

    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    REQUIRE(content.find("should not appear") == std::string::npos);
    REQUIRE(content.find("warning message") != std::string::npos);
    REQUIRE(content.find("error message") != std::string::npos);

    std::remove(path.c_str());
}

TEST_CASE("global_logger returns same instance", "[logger]") {
    Logger& a = global_logger();
    Logger& b = global_logger();
    REQUIRE(&a == &b);
}

TEST_CASE("Logger set_file returns false for invalid path", "[logger]") {
    Logger logger;
    REQUIRE_FALSE(logger.set_file("/nonexistent_dir_12345/test.log"));
}
