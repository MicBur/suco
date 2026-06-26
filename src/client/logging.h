#pragma once

#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

#define SUCO_LOG_INFO(msg, ...)  suco::Logger::info(msg, ##__VA_ARGS__)
#define SUCO_LOG_WARNING(msg, ...) suco::Logger::warn(msg, ##__VA_ARGS__)
#define SUCO_LOG_ERROR(msg, ...) suco::Logger::error(msg, ##__VA_ARGS__)

namespace suco {

class Logger {
public:
    template<typename... Args>
    static void info(const std::string& fmt, Args&&... args) {
        log("INFO", fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warn(const std::string& fmt, Args&&... args) {
        log("WARN", fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void error(const std::string& fmt, Args&&... args) {
        log("ERROR", fmt, std::forward<Args>(args)...);
    }

private:
    template<typename... Args>
    static void log(const std::string& level, const std::string& fmt, Args&&... args) {
        std::string msg = format(fmt, std::forward<Args>(args)...);
        std::cerr << "[SUCO " << level << "] " << msg << std::endl;
    }

    static std::string format(const std::string& fmt) {
        return fmt;
    }

    template<typename T, typename... Args>
    static std::string format(const std::string& fmt, T&& first, Args&&... rest) {
        size_t pos = fmt.find("{}");
        if (pos == std::string::npos) {
            return fmt;
        }

        std::string val;
        using DecayedT = std::decay_t<T>;
        if constexpr (std::is_convertible_v<DecayedT, std::string>) {
            val = std::string(std::forward<T>(first));
        } else if constexpr (std::is_constructible_v<std::string, DecayedT>) {
            val = std::string(std::forward<T>(first));
        } else {
            val = std::to_string(std::forward<T>(first));
        }

        std::string next_fmt = fmt.substr(0, pos) + val + fmt.substr(pos + 2);
        return format(next_fmt, std::forward<Args>(rest)...);
    }
};

} // namespace suco
