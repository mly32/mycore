#include "mycore/debug/log.hpp"

#include <memory>
#include <mutex>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>

namespace mycore::debug {
namespace {

std::mutex logging_mutex;
LogLevel configured_level = LogLevel::Info;

[[nodiscard]] spdlog::level::level_enum to_spdlog(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warning:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    case LogLevel::Off:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

[[nodiscard]] std::shared_ptr<spdlog::logger> category_logger(std::string_view category) {
    const auto name = category.empty() ? std::string{"mycore"} : std::string{category};
    if (auto existing = spdlog::get(name)) {
        return existing;
    }
    auto created = spdlog::stdout_color_mt(name);
    created->set_level(to_spdlog(configured_level));
    return created;
}

} // namespace

Runtime::Runtime(LogLevel level) {
    initialize_logging(level);
}

Runtime::~Runtime() {
    shutdown_logging();
}

void initialize_logging(LogLevel level) {
    const std::scoped_lock lock{logging_mutex};
    configured_level = level;
    spdlog::set_pattern("[%H:%M:%S.%e] [%n] [%^%l%$] %v");
    spdlog::set_level(to_spdlog(level));
    spdlog::flush_on(spdlog::level::warn);
}

void shutdown_logging() noexcept {
    try {
        const std::scoped_lock lock{logging_mutex};
        spdlog::shutdown();
    } catch (...) {
    }
}

void write_log(LogLevel level, std::string_view category, std::string_view message) {
    const std::scoped_lock lock{logging_mutex};
    category_logger(category)->log(to_spdlog(level), "{}", message);
}

} // namespace mycore::debug
