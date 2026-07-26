#pragma once

#include "dots/simulation/world_state.hpp"
#include "mycore/math/vector2.hpp"

#include <chrono>
#include <cstdint>

namespace dots::simulation {

inline constexpr std::uint32_t kTickRateHz = 30;
inline constexpr auto kTickDuration =
    std::chrono::nanoseconds{std::chrono::seconds{1}} / kTickRateHz;

[[nodiscard]] mycore::math::Vector2
normalized_player_movement(mycore::math::Vector2 desired_movement) noexcept;

// The movement argument must be the result of normalized_player_movement. Keeping normalization
// separate lets World retain the last installed desired movement when no new input is available.
[[nodiscard]] mycore::math::Vector2
advance_player_position(mycore::math::Vector2 position,
                        mycore::math::Vector2 normalized_movement,
                        float speed_units_per_second = kPlayerSpeedUnitsPerSecond) noexcept;

} // namespace dots::simulation
