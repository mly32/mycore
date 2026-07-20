#include "dots/server/server_runtime.hpp"
#include "dots/simulation/world_setup.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
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
  dots_server [--listen <address>] [--ticks <count>]
              [--fake-lag-ms <milliseconds>] [--fake-loss-percent <percent>] [--help]

Options:
  --listen <address>           Listen on a numeric IPv4 or bracketed IPv6 address.
                               Defaults to [::]:27020. Port 0 selects a private dynamic port.
  --ticks <count>              Run exactly this many simulation ticks, then exit.
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
    mycore::net_transport::NetworkImpairment impairment;
    bool help{};
};

std::uint32_t parse_unsigned(std::string_view value, std::string_view option) {
    std::uint32_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error{std::string{option} + " requires a non-negative integer"};
    }
    return result;
}

float parse_percent(std::string_view value, std::string_view option) {
    float result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result) ||
        result < 0.0F || result > 100.0F) {
        throw std::runtime_error{std::string{option} + " requires a number in the range 0..100"};
    }
    return result;
}

Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "--listen") {
            if (index + 1 >= argc) {
                throw std::runtime_error{"--listen requires an address"};
            }
            options.listen_address = argv[++index];
            continue;
        }
        if (argument == "--fake-lag-ms") {
            if (index + 1 >= argc) {
                throw std::runtime_error{"--fake-lag-ms requires a value"};
            }
            options.impairment.outgoing_lag_milliseconds =
                parse_unsigned(argv[++index], "--fake-lag-ms");
            continue;
        }
        if (argument == "--fake-loss-percent") {
            if (index + 1 >= argc) {
                throw std::runtime_error{"--fake-loss-percent requires a value"};
            }
            options.impairment.outgoing_loss_percent =
                parse_percent(argv[++index], "--fake-loss-percent");
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
        const auto listen_address =
            mycore::net_transport::NetworkAddress::parse(options.listen_address);
        if (!listen_address) {
            throw std::runtime_error{"--listen requires a numeric IPv4 or bracketed IPv6 address"};
        }
        mycore::net_transport::GameNetworkingSocketsNetwork network{options.impairment};
        const auto listening = network.listen(*listen_address);
        dots::server::Runtime server{*listening.endpoint, std::move(world)};

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
