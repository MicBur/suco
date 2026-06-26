#pragma once

#include <string>
#include <format>

#define SUCO_LOG_INFO(...)    suco::log_message("INFO", std::format(__VA_ARGS__))
#define SUCO_LOG_WARNING(...) suco::log_message("WARN", std::format(__VA_ARGS__))
#define SUCO_LOG_ERROR(...)   suco::log_message("ERROR", std::format(__VA_ARGS__))

namespace suco {

/**
 * @brief Logs a formatted message to stderr.
 * @param level The log level (e.g. "INFO", "WARN", "ERROR").
 * @param message The pre-formatted message string.
 */
void log_message(const std::string& level, const std::string& message);

} // namespace suco
