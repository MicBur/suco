#pragma once

#include <string>
#include <format>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

namespace suco {

// Sets the current log level (reads SUCO_LOG_LEVEL from env)
LogLevel get_log_level();
void set_log_level(LogLevel level);

// Check if a specific level is enabled
bool is_log_level_enabled(LogLevel level);

void log_message(LogLevel level, const std::string& message);

} // namespace suco

// Performance-optimized logging macros (only evaluate std::format if the log level is enabled)
#define SUCO_LOG_DEBUG(...) \
    do { \
        if (suco::is_log_level_enabled(LogLevel::DEBUG)) { \
            suco::log_message(LogLevel::DEBUG, std::format(__VA_ARGS__)); \
        } \
    } while (0)

#define SUCO_LOG_INFO(...) \
    do { \
        if (suco::is_log_level_enabled(LogLevel::INFO)) { \
            suco::log_message(LogLevel::INFO, std::format(__VA_ARGS__)); \
        } \
    } while (0)

#define SUCO_LOG_WARNING(...) \
    do { \
        if (suco::is_log_level_enabled(LogLevel::WARN)) { \
            suco::log_message(LogLevel::WARN, std::format(__VA_ARGS__)); \
        } \
    } while (0)

#define SUCO_LOG_ERROR(...) \
    do { \
        if (suco::is_log_level_enabled(LogLevel::ERROR)) { \
            suco::log_message(LogLevel::ERROR, std::format(__VA_ARGS__)); \
        } \
    } while (0)
