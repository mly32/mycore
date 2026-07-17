#pragma once

#include "dots/simulation/input_command.hpp"
#include "mycore/platform_sdl/input.hpp"

#include <string_view>
#include <vector>

namespace dots::client {

enum class InputMode {
    Mouse,
    Keyboard,
    Hybrid,
};

struct Bindings {
    std::vector<mycore::platform_sdl::Key> up;
    std::vector<mycore::platform_sdl::Key> down;
    std::vector<mycore::platform_sdl::Key> left;
    std::vector<mycore::platform_sdl::Key> right;
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

[[nodiscard]] std::string_view input_mode_name(InputMode mode) noexcept;

[[nodiscard]] bool quit_requested(const mycore::platform_sdl::InputSnapshot& input,
                                  const ClientControls& controls) noexcept;

[[nodiscard]] dots::simulation::InputCommand
make_input_command(const mycore::platform_sdl::InputSnapshot& input,
                   const ClientControls& controls,
                   dots::simulation::EntityId entity_id,
                   dots::simulation::InputCommandId command_id,
                   InputViewport viewport) noexcept;

} // namespace dots::client
