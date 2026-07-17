#include "dots/client/client_config.hpp"
#include "mycore/platform_sdl/input.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Catch::Matchers::ContainsSubstring;

class TempDirectory {
public:
    TempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("mycore-dots-config-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream stream{path};
    REQUIRE(stream);
    stream << contents;
    REQUIRE(stream);
}

} // namespace

TEST_CASE("Client configuration defaults match the playable client", "[dots][client][config]") {
    const auto config = dots::client::default_client_config();

    REQUIRE(config.window.title == "Dots");
    REQUIRE(config.window.width == 1280);
    REQUIRE(config.window.height == 720);
    REQUIRE(config.window.resizable);
    REQUIRE_FALSE(config.window.fullscreen);
    REQUIRE(config.window.high_dpi);
    REQUIRE(config.window.vsync);
    REQUIRE(config.controls.mode == dots::client::InputMode::Hybrid);
    REQUIRE(config.controls.mouse_dead_zone_pixels == 12.0F);
    REQUIRE(config.controls.bindings.up ==
            std::vector{mycore::platform_sdl::Key::W, mycore::platform_sdl::Key::Up});
    REQUIRE(config.controls.bindings.quit == std::vector{mycore::platform_sdl::Key::Escape});
    REQUIRE(config.simulation.max_frame_delta_ms == 250);
    REQUIRE(config.simulation.max_steps_per_frame == 5);
    REQUIRE(config.view.pixels_per_world_unit == 20.0F);
    REQUIRE(config.view.draw_grid);
    REQUIRE(config.colors.food == dots::client::RgbColor{0xF7, 0x25, 0x85});
}

TEST_CASE("Client configuration accepts complete TOML", "[dots][client][config]") {
    constexpr std::string_view document = R"(
[window]
title = "Configured Dots"
width = 1600
height = 900
resizable = false
fullscreen = true
high_dpi = false
vsync = false

[input]
mode = "keyboard"
mouse_dead_zone_pixels = 4.5

[bindings]
up = ["I"]
down = ["K"]
left = ["J"]
right = ["L"]
quit = ["Q"]

[simulation]
max_frame_delta_ms = 100
max_steps_per_frame = 3

[view]
pixels_per_world_unit = 32.0
draw_grid = false
grid_spacing_world_units = 4.0

[colors]
background = "#010203"
grid = "#a0B1c2"
player = "#112233"
food = "#FEDCBA"
)";
    const auto config = dots::client::parse_client_config(document, "complete.toml");

    REQUIRE(config.window.title == "Configured Dots");
    REQUIRE(config.window.width == 1600);
    REQUIRE(config.window.height == 900);
    REQUIRE_FALSE(config.window.resizable);
    REQUIRE(config.window.fullscreen);
    REQUIRE_FALSE(config.window.high_dpi);
    REQUIRE_FALSE(config.window.vsync);
    REQUIRE(config.controls.mode == dots::client::InputMode::Keyboard);
    REQUIRE(config.controls.mouse_dead_zone_pixels == 4.5F);
    REQUIRE(config.controls.bindings.up == std::vector{mycore::platform_sdl::Key::I});
    REQUIRE(config.simulation.max_frame_delta_ms == 100);
    REQUIRE(config.simulation.max_steps_per_frame == 3);
    REQUIRE(config.view.pixels_per_world_unit == 32.0F);
    REQUIRE_FALSE(config.view.draw_grid);
    REQUIRE(config.view.grid_spacing_world_units == 4.0F);
    REQUIRE(config.colors.background == dots::client::RgbColor{0x01, 0x02, 0x03});
    REQUIRE(config.colors.grid == dots::client::RgbColor{0xA0, 0xB1, 0xC2});
    REQUIRE(config.colors.food == dots::client::RgbColor{0xFE, 0xDC, 0xBA});
}

TEST_CASE("Partial TOML retains defaults and key names are case insensitive",
          "[dots][client][config]") {
    constexpr std::string_view document = R"(
[window]
width = 900

[input]
mode = "HyBrId"

[bindings]
up = ["w"]
down = ["s"]
left = ["a"]
right = ["d"]
quit = ["eScApE"]
)";
    const auto config = dots::client::parse_client_config(document, "partial.toml");

    REQUIRE(config.window.width == 900);
    REQUIRE(config.window.height == 720);
    REQUIRE(config.window.title == "Dots");
    REQUIRE(config.controls.mode == dots::client::InputMode::Hybrid);
    REQUIRE(config.controls.bindings.up == std::vector{mycore::platform_sdl::Key::W});
    REQUIRE(config.controls.bindings.quit == std::vector{mycore::platform_sdl::Key::Escape});
    REQUIRE(config.colors.background == dots::client::RgbColor{0x10, 0x18, 0x20});
}

TEST_CASE("Client configuration supports portable physical key categories",
          "[dots][client][config]") {
    constexpr std::string_view document = R"(
[bindings]
up = ["Z", "0", "Digit1", "Up", "Tab", "LeftShift", "RightControl"]
down = ["2", "Down", "Space", "RightShift", "LeftControl"]
left = ["3", "Left", "LeftAlt", "RightGui", "Insert", "Home", "PageUp", "F1", "F12"]
right = ["4", "Right", "RightAlt", "LeftGui", "Delete", "End", "PageDown", "F2", "F11"]
quit = ["Escape", "Enter", "Backspace", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10"]
)";
    const auto config = dots::client::parse_client_config(document, "keys.toml");

    REQUIRE(config.controls.bindings.up.front() == mycore::platform_sdl::Key::Z);
    REQUIRE(config.controls.bindings.up[1] == mycore::platform_sdl::Key::Digit0);
    REQUIRE(config.controls.bindings.up[2] == mycore::platform_sdl::Key::Digit1);
    REQUIRE(config.controls.bindings.left[6] == mycore::platform_sdl::Key::PageUp);
    REQUIRE(config.controls.bindings.left.back() == mycore::platform_sdl::Key::F12);
    REQUIRE(config.controls.bindings.right[3] == mycore::platform_sdl::Key::LeftGui);
    REQUIRE(config.controls.bindings.quit[1] == mycore::platform_sdl::Key::Enter);
}

TEST_CASE("Explicit config replaces automatic config and missing automatic config uses defaults",
          "[dots][client][config]") {
    TempDirectory directory;
    const auto automatic = directory.path() / "dots-client.toml";
    const auto explicit_path = directory.path() / "explicit.toml";
    write_file(automatic, "[window]\nwidth = 900\ntitle = \"Automatic\"\n");
    write_file(explicit_path, "[window]\nwidth = 700\n");

    const auto automatic_config = dots::client::load_client_config(std::nullopt, directory.path());
    REQUIRE(automatic_config.window.width == 900);
    REQUIRE(automatic_config.window.title == "Automatic");

    const auto explicit_config =
        dots::client::load_client_config(std::filesystem::path{"explicit.toml"}, directory.path());
    REQUIRE(explicit_config.window.width == 700);
    REQUIRE(explicit_config.window.title == "Dots");

    std::filesystem::remove(automatic);
    const auto defaults = dots::client::load_client_config(std::nullopt, directory.path());
    REQUIRE(defaults.window.width == 1280);
}

TEST_CASE("Missing explicit client configuration reports file and field",
          "[dots][client][config]") {
    TempDirectory directory;
    REQUIRE_THROWS_WITH(
        dots::client::load_client_config(std::filesystem::path{"missing.toml"}, directory.path()),
        ContainsSubstring("missing.toml") && ContainsSubstring("<file>"));
}

TEST_CASE("Invalid client TOML reports the source and field", "[dots][client][config]") {
    struct InvalidDocument {
        std::string_view text;
        std::string_view field;
    };
    const std::vector invalid_documents{
        InvalidDocument{"[window", "<document>"},
        InvalidDocument{"mystery = true", "mystery"},
        InvalidDocument{"[window]\nwidht = 10", "window.widht"},
        InvalidDocument{"window = 10", "window"},
        InvalidDocument{"[window]\nwidth = \"wide\"", "window.width"},
        InvalidDocument{"[window]\nwidth = 0", "window.width"},
        InvalidDocument{"[window]\nheight = 16385", "window.height"},
        InvalidDocument{"[input]\nmode = \"controller\"", "input.mode"},
        InvalidDocument{"[input]\nmouse_dead_zone_pixels = -1", "input.mouse_dead_zone_pixels"},
        InvalidDocument{"[bindings]\nup = []", "bindings.up"},
        InvalidDocument{"[bindings]\nup = [\"NotAKey\"]", "bindings.up"},
        InvalidDocument{"[bindings]\nup = [\"S\"]", "bindings.up"},
        InvalidDocument{"[simulation]\nmax_frame_delta_ms = 0", "simulation.max_frame_delta_ms"},
        InvalidDocument{"[simulation]\nmax_steps_per_frame = 0", "simulation.max_steps_per_frame"},
        InvalidDocument{"[view]\npixels_per_world_unit = 0", "view.pixels_per_world_unit"},
        InvalidDocument{"[view]\ngrid_spacing_world_units = -2", "view.grid_spacing_world_units"},
        InvalidDocument{"[colors]\nplayer = \"#12345Z\"", "colors.player"},
    };

    for (const auto& invalid : invalid_documents) {
        CAPTURE(invalid.text);
        REQUIRE_THROWS_WITH(dots::client::parse_client_config(invalid.text, "invalid.toml"),
                            ContainsSubstring("invalid.toml") &&
                                ContainsSubstring(std::string{invalid.field}));
    }
}
