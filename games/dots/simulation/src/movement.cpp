#include "dots/simulation/movement.hpp"

#include <cmath>

namespace dots::simulation {

mycore::math::Vector2 normalized_player_movement(mycore::math::Vector2 desired_movement) noexcept {
    return mycore::math::normalized_or_zero(desired_movement);
}

bool is_valid_player_movement(mycore::math::Vector2 movement) noexcept {
    constexpr float kLengthTolerance = 0.0001F;
    return std::isfinite(movement.x) && std::isfinite(movement.y) &&
           mycore::math::length_squared(movement) <= 1.0F + kLengthTolerance;
}

mycore::math::Vector2 advance_player_position(mycore::math::Vector2 position,
                                              mycore::math::Vector2 normalized_movement,
                                              float speed_units_per_second) noexcept {
    const auto distance_per_tick = speed_units_per_second / static_cast<float>(kTickRateHz);
    return position + (normalized_movement * distance_per_tick);
}

mycore::math::Vector2 decayed_launch_velocity(mycore::math::Vector2 launch_velocity,
                                              float decay_units_per_second_squared) noexcept {
    const auto speed = mycore::math::length(launch_velocity);
    const auto decay_per_tick = decay_units_per_second_squared / static_cast<float>(kTickRateHz);
    if (speed <= decay_per_tick) {
        return {};
    }
    return launch_velocity * ((speed - decay_per_tick) / speed);
}

mycore::math::Vector2 player_kinematic_velocity(mycore::math::Vector2 normalized_movement,
                                                mycore::math::Vector2 launch_velocity,
                                                const WorldRules& rules) noexcept {
    return (normalized_movement * rules.player_speed_units_per_second) + launch_velocity;
}

PlayerKinematicState advance_player_kinematics(PlayerKinematicState state,
                                               mycore::math::Vector2 normalized_movement,
                                               const WorldRules& rules,
                                               mycore::math::Vector2 additional_velocity) noexcept {
    const auto velocity =
        player_kinematic_velocity(normalized_movement, state.launch_velocity, rules) +
        additional_velocity;
    state.position += velocity / static_cast<float>(kTickRateHz);
    state.launch_velocity =
        decayed_launch_velocity(state.launch_velocity, rules.launch_decay_units_per_second_squared);
    return state;
}

} // namespace dots::simulation
