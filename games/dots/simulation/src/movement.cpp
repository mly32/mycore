#include "dots/simulation/movement.hpp"

namespace dots::simulation {

mycore::math::Vector2 normalized_player_movement(mycore::math::Vector2 desired_movement) noexcept {
    return mycore::math::normalized_or_zero(desired_movement);
}

mycore::math::Vector2 advance_player_position(mycore::math::Vector2 position,
                                              mycore::math::Vector2 normalized_movement) noexcept {
    constexpr auto kDistancePerTick = kPlayerSpeedUnitsPerSecond / static_cast<float>(kTickRateHz);
    return position + (normalized_movement * kDistancePerTick);
}

} // namespace dots::simulation
