#include "dots/app_cli/app_cli.hpp"
#include "dots/client_runtime/client_runtime.hpp"
#include "dots/client_runtime/command_timing.hpp"
#include "dots/simulation/movement.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/net_transport/net_transport.hpp"
#include "mycore/time/time.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kHelp = R"(Dots Bot
Connects to an authoritative Dots server and continuously moves in a wide rectangle.

Usage:
  dots_bot --connect <address> [--fake-lag-ms <milliseconds>]
           [--fake-loss-percent <percent>] [--spectate] [--ticks <count>] [--help]

Options:
  --connect <address>          Numeric IPv4 or bracketed IPv6 server address.
  --fake-lag-ms <milliseconds> Add outgoing one-way packet delay.
  --fake-loss-percent <value>  Drop this percentage of outgoing packets (0..100).
  --spectate                   Join without an owned player and receive snapshots only.
  --ticks <count>              Send this many movement inputs after the two-input prefill.
  --help                       Show this help and exit.

The bot is headless and cycles right, down, left, and up every four seconds.
)";

volatile std::sig_atomic_t stop_requested{};

void request_stop(int) {
    stop_requested = 1;
}

struct Options {
    std::string server_address;
    mycore::net_transport::NetworkImpairment impairment;
    std::optional<std::uint64_t> ticks;
    bool spectate{};
    bool help{};
};

[[nodiscard]] Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "--spectate") {
            options.spectate = true;
            continue;
        }
        if (argument == "--connect") {
            options.server_address =
                dots::app_cli::parse_connect_address(
                    dots::app_cli::require_option_value(index, argc, argv, argument), argument)
                    .value();
        } else if (dots::app_cli::consume_network_impairment_option(
                       argument, index, argc, argv, options.impairment)) {
            continue;
        } else if (argument == "--ticks") {
            options.ticks = dots::app_cli::parse_positive_u64(
                dots::app_cli::require_option_value(index, argc, argv, argument), argument);
        } else {
            throw std::runtime_error{"invalid argument: " + std::string{argument}};
        }
    }
    if (!options.help && options.server_address.empty()) {
        throw std::runtime_error{"--connect is required"};
    }
    if (options.spectate && options.ticks) {
        throw std::runtime_error{"--ticks cannot be used with --spectate"};
    }
    return options;
}

[[nodiscard]] mycore::math::Vector2 movement_for_tick(std::uint64_t tick) noexcept {
    constexpr std::uint64_t kSideDurationTicks =
        static_cast<std::uint64_t>(dots::simulation::kTickRateHz) * 4U;
    constexpr std::array<mycore::math::Vector2, 4> kDirections{{
        {1.0F, 0.0F},
        {0.0F, 1.0F},
        {-1.0F, 0.0F},
        {0.0F, -1.0F},
    }};
    return kDirections[(tick / kSideDurationTicks) % kDirections.size()];
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
        const auto server_address =
            dots::app_cli::parse_connect_address(options.server_address, "--connect");

        mycore::net_transport::GameNetworkingSocketsNetwork network{options.impairment};
        auto& endpoint = network.connect(server_address);
        dots::client_runtime::Runtime client{
            endpoint,
            {
                .requested_role = options.spectate ? dots::protocol::JoinRole::Spectator
                                                   : dots::protocol::JoinRole::Player,
            },
        };
        dots::client_runtime::CommandTimingController command_timing;
        const auto process_events = [&client, &command_timing] {
            const auto result = client.process_events();
            for (const auto& accepted : result.accepted_snapshots) {
                command_timing.observe_server_queue_depth(accepted.snapshot.pending_input_count);
            }
            // Bots have no presentation side effects, but every runtime composition root must
            // retire the bounded stream of post-commit prediction event batches.
            static_cast<void>(client.take_prediction_event_batches());
            return result.error;
        };
        const auto handshake_deadline = std::chrono::steady_clock::now() + 10s;
        while (client.state() != dots::client_runtime::State::Ready &&
               std::chrono::steady_clock::now() < handshake_deadline) {
            if (const auto error = process_events()) {
                throw std::runtime_error{
                    "The bot handshake failed: " +
                    std::string{dots::client_runtime::runtime_error_name(*error)}};
            }
            std::this_thread::sleep_for(1ms);
        }
        if (client.state() != dots::client_runtime::State::Ready) {
            throw std::runtime_error{"Could not establish the authoritative session"};
        }

        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        std::uint64_t next_client_tick{};
        if (!options.spectate) {
            for (std::size_t index = 0; index < command_timing.initial_prefill_count(); ++index) {
                if (client.submit_input(static_cast<std::uint32_t>(next_client_tick), {}) !=
                    dots::client_runtime::InputSendResult::Sent) {
                    throw std::runtime_error{"The bot could not prefill input"};
                }
                ++next_client_tick;
                command_timing.record_prefill_inputs(1);
            }
        }
        std::uint64_t movement_ticks{};
        auto next_tick = std::chrono::steady_clock::now();
        if (options.spectate) {
            mycore::debug::log_info(
                "dots.bot", "Connected as spectator client {}", client.client_id().value());
        } else {
            mycore::debug::log_info("dots.bot",
                                    "Connected as client {} controlling entity {}",
                                    client.client_id().value(),
                                    client.controlled_entity_id().value());
        }
        while (stop_requested == 0 && (!options.ticks || movement_ticks < *options.ticks)) {
            if (const auto error = process_events()) {
                throw std::runtime_error{
                    "The bot session failed: " +
                    std::string{dots::client_runtime::runtime_error_name(*error)}};
            }
            const auto produces_input =
                client.accepted_role() == dots::protocol::JoinRole::Player &&
                client.session_mode() == dots::protocol::SessionMode::Playing &&
                !client.input_production_paused();
            if (produces_input) {
                if (next_client_tick > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error{"Bot input ticks are exhausted"};
                }
                if (client.submit_input(static_cast<std::uint32_t>(next_client_tick),
                                        movement_for_tick(movement_ticks)) !=
                    dots::client_runtime::InputSendResult::Sent) {
                    throw std::runtime_error{"The bot could not submit input"};
                }
                ++next_client_tick;
                ++movement_ticks;
            }
            const auto period = produces_input
                                    ? command_timing.next_period(dots::simulation::kTickDuration)
                                    : dots::simulation::kTickDuration;
            const auto deadline = mycore::time::advance_periodic_deadline(
                next_tick, period, std::chrono::steady_clock::now());
            next_tick = deadline.time;
            if (deadline.discarded_backlog) {
                command_timing.record_discarded_backlog();
            }
            std::this_thread::sleep_until(next_tick);
        }
        static_cast<void>(client.disconnect());
        return 0;
    } catch (const std::exception& error) {
        mycore::debug::log_error("dots.bot", "{}", error.what());
        std::cerr << "dots_bot: " << error.what() << '\n';
        return 1;
    }
}
