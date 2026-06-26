#include "logging.h"

#include <chrono>
#include <ctime>
#include <iostream>
#include <format>

namespace suco {
namespace {

// Thread-safe and platform-independent helper to generate a formatted timestamp
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

} // namespace

void log_message(const std::string& level, const std::string& message) {
    std::string timestamp = get_current_timestamp();
    std::string formatted = std::format("{} [{}] {}\n", timestamp, level, message);

    if (level == "INFO") {
        std::cout << formatted << std::flush;
    } else {
        std::cerr << formatted << std::flush;
    }
}

} // namespace suco
