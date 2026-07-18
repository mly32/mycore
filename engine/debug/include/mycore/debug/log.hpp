#pragma once

#include <cstdint>
#include <fmt/format.h>
#include <string_view>
#include <utility>

namespace mycore::debug {

enum class LogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off,
};

class Runtime {
public:
    explicit Runtime(LogLevel level = LogLevel::Info);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
};

void initialize_logging(LogLevel level = LogLevel::Info);
void shutdown_logging() noexcept;
void write_log(LogLevel level, std::string_view category, std::string_view message);

template <typename... Args>
void log(LogLevel level,
         std::string_view category,
         fmt::format_string<Args...> format,
         Args&&... args) {
    write_log(level, category, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void log_info(std::string_view category, fmt::format_string<Args...> format, Args&&... args) {
    log(LogLevel::Info, category, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_warning(std::string_view category, fmt::format_string<Args...> format, Args&&... args) {
    log(LogLevel::Warning, category, format, std::forward<Args>(args)...);
}

template <typename... Args>
void log_error(std::string_view category, fmt::format_string<Args...> format, Args&&... args) {
    log(LogLevel::Error, category, format, std::forward<Args>(args)...);
}

} // namespace mycore::debug
