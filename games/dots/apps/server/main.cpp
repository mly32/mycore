#include "dots/server/server_runtime.hpp"
#include "dots/simulation/world_setup.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

constexpr std::string_view kHelp = R"(Dots Server
Runs the authoritative Dots simulation at 30 Hz.

Usage:
  dots_server [--ticks <count>] [--help]

Options:
  --ticks <count>  Run exactly this many simulation ticks, then exit.
  --help           Show this help text and exit.

The Feature 09 server is headless and uses the in-memory transport contract. Cross-process
client connections arrive with the native transport in Feature 10.
)";

volatile std::sig_atomic_t stop_requested{};

void request_stop(int) {
    stop_requested = 1;
}

struct Options {
    std::optional<std::uint64_t> ticks;
    bool help{};
};

Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument != "--ticks" || options.ticks || index + 1 >= argc) {
            throw std::runtime_error{"invalid argument: " + std::string{argument}};
        }
        const std::string_view value{argv[++index]};
        std::uint64_t ticks{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), ticks);
        if (error != std::errc{} || end != value.data() + value.size() || ticks == 0) {
            throw std::runtime_error{"--ticks requires a positive integer"};
        }
        options.ticks = ticks;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const mycore::debug::Runtime logging;
    try {
        const auto options = parse_arguments(argc, argv);
        if (options.help) {
            std::cout << kHelp;
            return 0;
        }

        dots::simulation::World world;
        if (!dots::simulation::spawn_default_food_field(world)) {
            throw std::runtime_error{"Could not populate the authoritative Dots world"};
        }
        mycore::net_transport::InMemoryNetwork network;
        dots::server::Runtime server{network.server_endpoint(), std::move(world)};

        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        auto next_tick = std::chrono::steady_clock::now();
        std::uint64_t completed_ticks{};
        mycore::debug::log_info("dots.server", "Authoritative 30 Hz server started");
        while (stop_requested == 0 && (!options.ticks || completed_ticks < *options.ticks)) {
            if (server.process_events() || server.step()) {
                throw std::runtime_error{
                    "The authoritative Dots server encountered a runtime error"};
            }
            ++completed_ticks;
            next_tick += dots::simulation::kTickDuration;
            std::this_thread::sleep_until(next_tick);
        }
        mycore::debug::log_info("dots.server", "Stopped after {} ticks", completed_ticks);
        return 0;
    } catch (const std::exception& error) {
        mycore::debug::log_error("dots.server", "{}", error.what());
        std::cerr << "dots_server: " << error.what() << '\n';
        return 1;
    }
}
