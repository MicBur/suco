#include "logging.h"

#include <chrono>
#include <ctime>
#include <iostream>
#include <cstdlib>
#include <mutex>
#include <algorithm>

namespace suco {
namespace {

std::string get_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};

#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    return std::format("[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}]",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

const char* log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "INFO";
}

LogLevel parse_log_level(std::string str) {
    // Convert to uppercase
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return std::toupper(c);
    });

    if (str == "DEBUG") return LogLevel::DEBUG;
    if (str == "INFO")  return LogLevel::INFO;
    if (str == "WARN" || str == "WARNING")  return LogLevel::WARN;
    if (str == "ERROR") return LogLevel::ERROR;
    return LogLevel::INFO;
}

struct LoggerState {
    LogLevel current_level = LogLevel::INFO;
    bool initialized = false;
    std::mutex mutex;
};

LoggerState& get_logger_state() {
    static LoggerState state;
    if (!state.initialized) {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.initialized) {
            const char* env_val = std::getenv("SUCO_LOG_LEVEL");
            if (env_val) {
                state.current_level = parse_log_level(env_val);
            } else {
                state.current_level = LogLevel::INFO;
            }
            state.initialized = true;
        }
    }
    return state;
}

} // namespace

LogLevel get_log_level() {
    return get_logger_state().current_level;
}

void set_log_level(LogLevel level) {
    get_logger_state().current_level = level;
}

bool is_log_level_enabled(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(get_logger_state().current_level);
}

void log_message(LogLevel level, const std::string& message) {
    std::string timestamp = get_current_timestamp();
    std::string formatted = std::format("{} [{}] {}\n", timestamp, log_level_to_string(level), message);

    // Thread-safe output to stderr
    std::lock_guard<std::mutex> lock(get_logger_state().mutex);
    std::cerr << formatted << std::flush;
}

} // namespace suco
