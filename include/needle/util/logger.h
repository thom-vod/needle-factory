#pragma once
#include <string>
#include <mutex>
#include <cstdio>

// Windows headers #define ERROR 0 — undefine to avoid collision with our enum
#ifdef ERROR
#undef ERROR
#endif

namespace needle {

enum class LogLevel { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4 };

const char* log_level_name(LogLevel level);

class Logger {
public:
    Logger();
    void set_level(LogLevel level);
    LogLevel level() const;
    bool set_file(const std::string& path);
    void close_file();
    void log(LogLevel level, const char* category, const char* fmt, ...);
    bool is_enabled(LogLevel level) const;

private:
    void write_line(LogLevel level, const char* category, const char* msg);
    LogLevel level_;
    std::mutex mutex_;
    FILE* file_;
};

Logger& global_logger();

// Macros with is_enabled() guard to prevent argument evaluation
#define NEEDLE_LOG_TRACE(cat, ...) \
    do { if (::needle::global_logger().is_enabled(::needle::LogLevel::TRACE)) \
             ::needle::global_logger().log(::needle::LogLevel::TRACE, cat, __VA_ARGS__); \
    } while (0)

#define NEEDLE_LOG_DEBUG(cat, ...) \
    do { if (::needle::global_logger().is_enabled(::needle::LogLevel::DEBUG)) \
             ::needle::global_logger().log(::needle::LogLevel::DEBUG, cat, __VA_ARGS__); \
    } while (0)

#define NEEDLE_LOG_INFO(cat, ...) \
    do { if (::needle::global_logger().is_enabled(::needle::LogLevel::INFO)) \
             ::needle::global_logger().log(::needle::LogLevel::INFO, cat, __VA_ARGS__); \
    } while (0)

#define NEEDLE_LOG_WARN(cat, ...) \
    do { if (::needle::global_logger().is_enabled(::needle::LogLevel::WARN)) \
             ::needle::global_logger().log(::needle::LogLevel::WARN, cat, __VA_ARGS__); \
    } while (0)

// Use static_cast to avoid Windows ERROR macro collision at call sites
#define NEEDLE_LOG_ERROR(cat, ...) \
    do { if (::needle::global_logger().is_enabled(static_cast<::needle::LogLevel>(4))) \
             ::needle::global_logger().log(static_cast<::needle::LogLevel>(4), cat, __VA_ARGS__); \
    } while (0)

} // namespace needle
