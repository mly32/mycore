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

enum class PresentationMode : std::uint8_t {
    Interpolated,
    Fixed,
    Comparison,
};

enum class NetworkMode : std::uint8_t {
    Offline,
    InMemory,
    Native,
};

[[nodiscard]] constexpr std::string_view network_mode_name(NetworkMode mode) noexcept {
    switch (mode) {
    case NetworkMode::Offline:
        return "OFFLINE";
    case NetworkMode::InMemory:
        return "IN MEMORY";
    case NetworkMode::Native:
        return "NATIVE";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view presentation_mode_name(PresentationMode mode) noexcept {
    switch (mode) {
    case PresentationMode::Interpolated:
        return "INTERPOLATED";
    case PresentationMode::Fixed:
        return "FIXED";
    case PresentationMode::Comparison:
        return "COMPARISON";
    }
    return "UNKNOWN";
}

class StartupError : public std::runtime_error {
public:
    explicit StartupError(const std::string& message);
    StartupError(const std::filesystem::path& file, std::string_view field, std::string detail);
};

// Keep games/dots/config/dots-client.schema.json synchronized with these settings and with
// client_config.cpp whenever accepted fields, types, defaults, or validation rules change.
struct WindowSettings {
    std::string title{"Dots"};
    int width{1280};
    int height{720};
    bool resizable{true};
    bool fullscreen{};
    bool high_dpi{true};
    bool vsync{true};
};

struct NetworkSettings {
    NetworkMode mode{NetworkMode::Offline};
    std::string server_address{"127.0.0.1:27020"};
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

struct DebugSettings {
    PresentationMode presentation_mode{PresentationMode::Interpolated};
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
    RgbColor player_growth{0xFF, 0xD1, 0x66};
    RgbColor food{0xF7, 0x25, 0x85};
};

struct ClientConfig {
    WindowSettings window;
    NetworkSettings network;
    ClientControls controls;
    SimulationSettings simulation;
    ViewSettings view;
    DebugSettings debug;
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
