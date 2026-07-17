#pragma once

#include "dots/client/controls.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dots::client {

class StartupError : public std::runtime_error {
public:
    explicit StartupError(std::string message);
    StartupError(const std::filesystem::path& file, std::string_view field, std::string detail);
};

struct WindowSettings {
    std::string title{"Dots"};
    int width{1280};
    int height{720};
    bool resizable{true};
    bool fullscreen{};
    bool high_dpi{true};
    bool vsync{true};
};

struct SimulationSettings {
    int max_frame_delta_ms{250};
    std::size_t max_steps_per_frame{5};
};

struct ViewSettings {
    float pixels_per_world_unit{20.0F};
    bool draw_grid{true};
    float grid_spacing_world_units{8.0F};
};

struct RgbColor {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};

    auto operator<=>(const RgbColor&) const = default;
};

struct ColorSettings {
    RgbColor background{0x10, 0x18, 0x20};
    RgbColor grid{0x20, 0x30, 0x40};
    RgbColor player{0x4C, 0xC9, 0xF0};
    RgbColor food{0xF7, 0x25, 0x85};
};

struct ClientConfig {
    WindowSettings window;
    ClientControls controls;
    SimulationSettings simulation;
    ViewSettings view;
    ColorSettings colors;
};

[[nodiscard]] ClientConfig default_client_config();
[[nodiscard]] ClientConfig parse_client_config(std::string_view toml_text,
                                               const std::filesystem::path& source = "<memory>");
[[nodiscard]] ClientConfig
load_client_config(const std::optional<std::filesystem::path>& explicit_path,
                   const std::filesystem::path& current_directory);
[[nodiscard]] ClientConfig
load_client_config(const std::optional<std::filesystem::path>& explicit_path = std::nullopt);

} // namespace dots::client
