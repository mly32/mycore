#include "dots/client/client_app.hpp"
#include "dots/client/client_config.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kHelp = R"(Dots Client
A playable offline SDL client that runs the local Dots simulation.

Usage:
  dots_client [--config <path>] [--headless-smoke] [--help]

Options:
  --config <path>   Load this TOML file instead of automatic dots-client.toml.
  --headless-smoke  Initialize SDL and a hidden window, poll input once, then exit.
                    This does not create a renderer or start the game loop.
  --help            Show this help text and exit.

Controls:
  WASD / arrows     Move the player in keyboard or hybrid mode.
  Mouse             Move toward the cursor in mouse or hybrid mode.
                    The player stops while the cursor is inside its circle.
  Escape            Quit with the default bindings.

Configuration:
  Without --config, dots-client.toml in the current directory is loaded when present.
  Otherwise the built-in defaults are used.
)";

class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CliOptions {
    std::optional<std::filesystem::path> config_path;
    bool headless_smoke{};
    bool help{};
};

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
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_arguments(argc, argv);
        if (options.help) {
            std::cout << kHelp;
            return 0;
        }
        const auto config = dots::client::load_client_config(options.config_path);
        return dots::client::run_client(config, options.headless_smoke);
    } catch (const CliError& error) {
        std::cerr << "dots_client: " << error.what() << "\n\n" << kHelp;
    } catch (const std::exception& error) {
        std::cerr << "dots_client: " << error.what() << '\n';
    }
    return 1;
}
