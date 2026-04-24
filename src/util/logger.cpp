#include "needle/util/logger.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace needle {

const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

Logger::Logger()
    : level_(LogLevel::INFO)
    , file_(nullptr) {}

void Logger::set_level(LogLevel level) {
    level_ = level;
}

LogLevel Logger::level() const {
    return level_;
}

bool Logger::is_enabled(LogLevel level) const {
    return level >= level_;
}

bool Logger::set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    file_ = std::fopen(path.c_str(), "a");
    return file_ != nullptr;
}

void Logger::close_file() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void Logger::log(LogLevel level, const char* category, const char* fmt, ...) {
    if (!is_enabled(level)) return;

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    write_line(level, category, buf);
}

void Logger::write_line(LogLevel level, const char* category, const char* msg) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Write to stderr: [LEVEL] [category] message
    std::fprintf(stderr, "[%s] [%s] %s\n", log_level_name(level), category, msg);
    std::fflush(stderr);

    // Write to file with ISO 8601 timestamp if open
    if (file_) {
        std::time_t now = std::time(nullptr);
        struct std::tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &now);
#else
        gmtime_r(&now, &tm_buf);
#endif
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

        std::fprintf(file_, "%s [%s] [%s] %s\n", ts, log_level_name(level), category, msg);
        std::fflush(file_);
    }
}

Logger& global_logger() {
    static Logger instance;
    return instance;
}

} // namespace needle
