#include "dots/client/client_app.hpp"
#include "dots/client/client_config.hpp"
#include "mycore/debug/log.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kHelp = R"(Dots Client
A playable SDL_GPU client with offline, embedded-authority, and native-network modes.

Usage:
  dots_client [--config <path>] [--offline | --in-memory | --connect <address>]
              [--fake-lag-ms <milliseconds>] [--fake-loss-percent <percent>]
              [--headless-smoke | --package-smoke] [--help]

Options:
  --config <path>              Load this TOML file instead of automatic dots-client.toml.
  --offline                    Override configuration and run the local offline simulation.
  --in-memory                  Run an embedded authoritative server.
  --connect <address>          Connect to a numeric IPv4 or bracketed IPv6 server address.
  --fake-lag-ms <milliseconds> Add outgoing one-way packet delay in native mode.
  --fake-loss-percent <value>  Drop this percentage of outgoing packets (0..100).
  --headless-smoke             Initialize SDL and a hidden window, poll input once, then exit.
  --package-smoke              Also read every packaged shader without creating a GPU device.
  --help                       Show this help text and exit.

Controls:
  WASD / arrows     Move the player in keyboard or hybrid mode.
  Mouse             Move toward the cursor in mouse or hybrid mode.
                    The player stops while the cursor is inside its circle.
  Escape            Quit with the default bindings.

Configuration:
  Without --config, dots-client.toml in the current directory is loaded when present.
  Otherwise the built-in defaults are used.
  network.mode selects offline, in_memory, or native; CLI mode options take precedence.
  debug.presentation_mode selects offline interpolated, fixed, or comparison presentation.
  Networked modes present the latest authoritative snapshot without interpolation.
)";

class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CliOptions {
    std::optional<std::filesystem::path> config_path;
    bool headless_smoke{};
    bool package_smoke{};
    std::optional<dots::client::NetworkMode> network_mode;
    std::optional<std::string> connect_address;
    mycore::net_transport::NetworkImpairment impairment;
    bool impairment_specified{};
    bool help{};
};

std::uint32_t parse_unsigned(std::string_view value, std::string_view option) {
    std::uint32_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw CliError{std::string{option} + " requires a non-negative integer"};
    }
    return result;
}

float parse_percent(std::string_view value, std::string_view option) {
    float result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result) ||
        result < 0.0F || result > 100.0F) {
        throw CliError{std::string{option} + " requires a number in the range 0..100"};
    }
    return result;
}

void select_network_mode(CliOptions& options, dots::client::NetworkMode mode) {
    if (options.network_mode) {
        throw CliError{"--offline, --in-memory, and --connect are mutually exclusive"};
    }
    options.network_mode = mode;
}

CliOptions parse_arguments(int argc, char** argv) {
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "--headless-smoke") {
            options.headless_smoke = true;
            continue;
        }
        if (argument == "--package-smoke") {
            options.package_smoke = true;
            continue;
        }
        if (argument == "--in-memory") {
            select_network_mode(options, dots::client::NetworkMode::InMemory);
            continue;
        }
        if (argument == "--offline") {
            select_network_mode(options, dots::client::NetworkMode::Offline);
            continue;
        }
        if (argument == "--connect") {
            if (index + 1 >= argc) {
                throw CliError{"--connect requires an address"};
            }
            select_network_mode(options, dots::client::NetworkMode::Native);
            options.connect_address = argv[++index];
            const auto address =
                mycore::net_transport::NetworkAddress::parse(*options.connect_address);
            if (!address || address->port() == 0) {
                throw CliError{
                    "--connect requires a numeric IPv4 or bracketed IPv6 address with a port"};
            }
            options.connect_address = address->value();
            continue;
        }
        if (argument == "--fake-lag-ms") {
            if (index + 1 >= argc) {
                throw CliError{"--fake-lag-ms requires a value"};
            }
            options.impairment.outgoing_lag_milliseconds =
                parse_unsigned(argv[++index], "--fake-lag-ms");
            options.impairment_specified = true;
            continue;
        }
        if (argument == "--fake-loss-percent") {
            if (index + 1 >= argc) {
                throw CliError{"--fake-loss-percent requires a value"};
            }
            options.impairment.outgoing_loss_percent =
                parse_percent(argv[++index], "--fake-loss-percent");
            options.impairment_specified = true;
            continue;
        }
        if (argument == "--config") {
            if (options.config_path) {
                throw CliError{"--config may only be specified once"};
            }
            if (index + 1 >= argc) {
                throw CliError{"--config requires a path"};
            }
            const std::string_view path_argument{argv[index + 1]};
            if (path_argument.empty() || path_argument.starts_with("--")) {
                throw CliError{"--config requires a path"};
            }
            options.config_path = std::filesystem::path{argv[++index]};
            continue;
        }
        throw CliError{"unknown argument: " + std::string{argument}};
    }
    if (options.headless_smoke && options.package_smoke) {
        throw CliError{"--headless-smoke and --package-smoke are mutually exclusive"};
    }
    if ((options.headless_smoke || options.package_smoke) &&
        (options.network_mode || options.impairment_specified)) {
        throw CliError{"network mode and impairment options cannot be used with smoke modes"};
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
        auto config = dots::client::load_client_config(options.config_path);
        const auto network_mode = options.network_mode.value_or(config.network.mode);
        dots::client::ClientRunOptions run_options{
            .server_address = options.connect_address.value_or(config.network.server_address),
            .impairment = options.impairment,
        };
        if (options.headless_smoke) {
            run_options.mode = dots::client::ClientRunMode::HeadlessSmoke;
        } else if (options.package_smoke) {
            run_options.mode = dots::client::ClientRunMode::PackageSmoke;
        } else {
            switch (network_mode) {
            case dots::client::NetworkMode::Offline:
                run_options.mode = dots::client::ClientRunMode::Game;
                break;
            case dots::client::NetworkMode::InMemory:
                run_options.mode = dots::client::ClientRunMode::InMemoryGame;
                break;
            case dots::client::NetworkMode::Native:
                run_options.mode = dots::client::ClientRunMode::NativeGame;
                break;
            }
        }
        if (options.impairment_specified &&
            run_options.mode != dots::client::ClientRunMode::NativeGame) {
            throw CliError{"fake lag and loss options require native mode"};
        }
        return dots::client::run_client(config, run_options);
    } catch (const CliError& error) {
        mycore::debug::log_error("dots.client", "{}", error.what());
        std::cerr << "dots_client: " << error.what() << "\n\n" << kHelp;
    } catch (const std::exception& error) {
        mycore::debug::log_error("dots.client", "{}", error.what());
        std::cerr << "dots_client: " << error.what() << '\n';
    }
    return 1;
}
