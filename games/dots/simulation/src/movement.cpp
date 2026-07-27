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

} // namespace dots::simulation
