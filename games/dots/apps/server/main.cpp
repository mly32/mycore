#include "dots/app_cli/app_cli.hpp"
#include "dots/server/server_runtime.hpp"
#include "dots/simulation/world_setup.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::string_view kHelp = R"(Dots Server
Runs the authoritative Dots simulation at 30 Hz.

Usage:
  dots_server [--listen <address>] [--ticks <count>]
              [--respawn-cooldown-ticks <count>]
              [--fake-lag-ms <milliseconds>] [--fake-loss-percent <percent>] [--help]

Options:
  --listen <address>           Listen on a numeric IPv4 or bracketed IPv6 address.
                               Defaults to [::]:27020. Port 0 selects a private dynamic port.
  --ticks <count>              Run exactly this many simulation ticks, then exit.
  --respawn-cooldown-ticks <n> Allow respawn this many server ticks after defeat (default 90).
  --fake-lag-ms <milliseconds> Add outgoing one-way packet delay in native mode.
  --fake-loss-percent <value>  Drop this percentage of outgoing packets (0..100).
  --help                       Show this help text and exit.

The server is authoritative and headless. It prints DOTS_SERVER_READY after binding.
)";

volatile std::sig_atomic_t stop_requested{};

void request_stop(int) {
    stop_requested = 1;
}

struct Options {
    std::optional<std::uint64_t> ticks;
    std::string listen_address{"[::]:27020"};
    std::uint32_t respawn_cooldown_ticks{dots::server::kDefaultRespawnCooldownTicks};
    mycore::net_transport::NetworkImpairment impairment;
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
        if (argument == "--listen") {
            options.listen_address =
                dots::app_cli::require_option_value(index, argc, argv, argument);
            continue;
        }
        if (dots::app_cli::consume_network_impairment_option(
                argument, index, argc, argv, options.impairment)) {
            continue;
        }
        if (argument == "--respawn-cooldown-ticks") {
            options.respawn_cooldown_ticks = dots::app_cli::parse_nonnegative_u32(
                dots::app_cli::require_option_value(index, argc, argv, argument), argument);
            continue;
        }
        if (argument != "--ticks" || options.ticks) {
            throw std::runtime_error{"invalid argument: " + std::string{argument}};
        }
        options.ticks = dots::app_cli::parse_positive_u64(
            dots::app_cli::require_option_value(index, argc, argv, argument), argument);
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
        const auto listen_address =
            dots::app_cli::parse_listen_address(options.listen_address, "--listen");
        mycore::net_transport::GameNetworkingSocketsNetwork network{options.impairment};
        const auto listening = network.listen(listen_address);
        dots::server::Runtime server{
            *listening.endpoint,
            std::move(world),
            {.respawn_cooldown_ticks = options.respawn_cooldown_ticks},
        };

        std::cout << "DOTS_SERVER_READY " << listening.address.value() << '\n' << std::flush;

        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        auto next_tick = std::chrono::steady_clock::now();
        std::uint64_t completed_ticks{};
        mycore::debug::log_info(
            "dots.server", "Authoritative 30 Hz server listening on {}", listening.address.value());
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
