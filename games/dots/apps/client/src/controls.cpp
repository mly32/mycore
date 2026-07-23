#include "dots/client/controls.hpp"

#include "mycore/math/vector2.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace dots::client {
namespace {

using mycore::math::Vector2;
using mycore::platform_sdl::Key;
using mycore::platform_sdl::KeyboardSnapshot;

constexpr float kMaximumPendingWheelSteps = 64.0F;

bool any_pressed(const KeyboardSnapshot& keyboard, std::span<const Key> keys) noexcept {
    for (const auto key : keys) {
        if (keyboard.pressed(key)) {
            return true;
        }
    }
    return false;
}

Vector2 keyboard_movement(const KeyboardSnapshot& keyboard, const Bindings& bindings) noexcept {
    const auto horizontal = static_cast<float>(any_pressed(keyboard, bindings.right)) -
                            static_cast<float>(any_pressed(keyboard, bindings.left));
    const auto vertical = static_cast<float>(any_pressed(keyboard, bindings.down)) -
                          static_cast<float>(any_pressed(keyboard, bindings.up));
    return mycore::math::normalized_or_zero({horizontal, vertical});
}

Vector2 mouse_movement(const mycore::platform_sdl::MouseSnapshot& mouse,
                       float dead_zone,
                       InputViewport viewport) noexcept {
    if (viewport.width <= 0.0F || viewport.height <= 0.0F || viewport.mouse_scale_x <= 0.0F ||
        viewport.mouse_scale_y <= 0.0F) {
        return {};
    }

    const Vector2 direction{
        (mouse.x() * viewport.mouse_scale_x) - (viewport.width * 0.5F),
        (mouse.y() * viewport.mouse_scale_y) - (viewport.height * 0.5F),
    };
    const auto effective_dead_zone = std::max(dead_zone, viewport.player_radius_pixels);
    if (mycore::math::length_squared(direction) <= effective_dead_zone * effective_dead_zone) {
        return {};
    }
    return mycore::math::normalized_or_zero(direction);
}

} // namespace

SpectatorControlIntent
SpectatorControlTracker::sample(const mycore::platform_sdl::InputSnapshot& input,
                                const ClientControls& controls) noexcept {
    const auto follow_pressed = any_pressed(input.keyboard, controls.bindings.follow);
    const auto respawn_pressed = any_pressed(input.keyboard, controls.bindings.respawn);
    const auto zoom_in_pressed = any_pressed(input.keyboard, controls.bindings.zoom_in);
    const auto zoom_out_pressed = any_pressed(input.keyboard, controls.bindings.zoom_out);

    if (std::isfinite(input.wheel_delta_y)) {
        pending_wheel_delta_ = std::clamp(pending_wheel_delta_ + input.wheel_delta_y,
                                          -kMaximumPendingWheelSteps,
                                          kMaximumPendingWheelSteps);
    }
    const auto wheel_steps = static_cast<int>(std::trunc(pending_wheel_delta_));
    pending_wheel_delta_ -= static_cast<float>(wheel_steps);

    const SpectatorControlIntent intent{
        .pan = keyboard_movement(input.keyboard, controls.bindings),
        .zoom_steps = wheel_steps + static_cast<int>(zoom_in_pressed && !zoom_in_pressed_) -
                      static_cast<int>(zoom_out_pressed && !zoom_out_pressed_),
        .toggle_follow = follow_pressed && !follow_pressed_,
        .request_respawn = respawn_pressed && !respawn_pressed_,
    };
    follow_pressed_ = follow_pressed;
    respawn_pressed_ = respawn_pressed;
    zoom_in_pressed_ = zoom_in_pressed;
    zoom_out_pressed_ = zoom_out_pressed;
    return intent;
}

std::string_view input_mode_name(InputMode mode) noexcept {
    switch (mode) {
    case InputMode::Mouse:
        return "MOUSE";
    case InputMode::Keyboard:
        return "KEYBOARD";
    case InputMode::Hybrid:
        return "HYBRID";
    }
    return "UNKNOWN";
}

bool quit_requested(const mycore::platform_sdl::InputSnapshot& input,
                    const ClientControls& controls) noexcept {
    return input.quit_requested || any_pressed(input.keyboard, controls.bindings.quit);
}

dots::simulation::InputCommand make_input_command(const mycore::platform_sdl::InputSnapshot& input,
                                                  const ClientControls& controls,
                                                  dots::simulation::EntityId entity_id,
                                                  dots::simulation::InputCommandId command_id,
                                                  InputViewport viewport,
                                                  bool mouse_input_available) noexcept {
    return {
        .id = command_id,
        .entity_id = entity_id,
        .movement = movement_from_input(input, controls, viewport, mouse_input_available),
    };
}

mycore::math::Vector2 movement_from_input(const mycore::platform_sdl::InputSnapshot& input,
                                          const ClientControls& controls,
                                          InputViewport viewport,
                                          bool mouse_input_available) noexcept {
    const auto keyboard = keyboard_movement(input.keyboard, controls.bindings);
    const auto mouse = mouse_input_available
                           ? mouse_movement(input.mouse, controls.mouse_dead_zone_pixels, viewport)
                           : Vector2{};

    auto movement = mouse;
    switch (controls.mode) {
    case InputMode::Mouse:
        movement = mouse;
        break;
    case InputMode::Keyboard:
        movement = keyboard;
        break;
    case InputMode::Hybrid:
        movement = mycore::math::length_squared(keyboard) > 0.0F ? keyboard : mouse;
        break;
    }

    return movement;
}

} // namespace dots::client
