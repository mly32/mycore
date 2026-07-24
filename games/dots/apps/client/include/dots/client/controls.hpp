#pragma once

#include "dots/simulation/input_command.hpp"
#include "mycore/platform_sdl/input.hpp"

#include <string_view>
#include <vector>

namespace dots::client {

enum class InputMode : std::uint8_t {
    Mouse,
    Keyboard,
    Hybrid,
};

struct Bindings {
    std::vector<mycore::platform_sdl::Key> up;
    std::vector<mycore::platform_sdl::Key> down;
    std::vector<mycore::platform_sdl::Key> left;
    std::vector<mycore::platform_sdl::Key> right;
    std::vector<mycore::platform_sdl::Key> follow;
    std::vector<mycore::platform_sdl::Key> respawn;
    std::vector<mycore::platform_sdl::Key> zoom_in;
    std::vector<mycore::platform_sdl::Key> zoom_out;
    std::vector<mycore::platform_sdl::Key> quit;
};

struct ClientControls {
    InputMode mode{InputMode::Hybrid};
    float mouse_dead_zone_pixels{12.0F};
    Bindings bindings;
};

struct InputViewport {
    float width{};
    float height{};
    float mouse_scale_x{1.0F};
    float mouse_scale_y{1.0F};
    float player_radius_pixels{};
};

struct SpectatorControlIntent {
    mycore::math::Vector2 pan;
    int zoom_steps{};
    bool toggle_follow{};
    bool request_respawn{};
};

class SpectatorControlTracker {
public:
    [[nodiscard]] SpectatorControlIntent sample(const mycore::platform_sdl::InputSnapshot& input,
                                                const ClientControls& controls) noexcept;

private:
    float pending_wheel_delta_{};
    bool follow_pressed_{};
    bool respawn_pressed_{};
    bool zoom_in_pressed_{};
    bool zoom_out_pressed_{};
};

[[nodiscard]] std::string_view input_mode_name(InputMode mode) noexcept;

[[nodiscard]] bool quit_requested(const mycore::platform_sdl::InputSnapshot& input,
                                  const ClientControls& controls) noexcept;

[[nodiscard]] mycore::math::Vector2
movement_from_input(const mycore::platform_sdl::InputSnapshot& input,
                    const ClientControls& controls,
                    InputViewport viewport,
                    bool mouse_input_available = true) noexcept;

[[nodiscard]] dots::simulation::InputCommand
make_input_command(const mycore::platform_sdl::InputSnapshot& input,
                   const ClientControls& controls,
                   dots::simulation::EntityId entity_id,
                   dots::simulation::InputCommandId command_id,
                   InputViewport viewport,
                   bool mouse_input_available = true) noexcept;

} // namespace dots::client
