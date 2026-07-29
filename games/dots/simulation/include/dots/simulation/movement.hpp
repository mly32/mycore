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

[[nodiscard]] bool is_valid_player_movement(mycore::math::Vector2 movement) noexcept;

// The movement argument must be the result of normalized_player_movement. Keeping normalization
// separate lets World retain the last installed desired movement when no new input is available.
[[nodiscard]] mycore::math::Vector2
advance_player_position(mycore::math::Vector2 position,
                        mycore::math::Vector2 normalized_movement,
                        float speed_units_per_second = kPlayerSpeedUnitsPerSecond) noexcept;

struct PlayerKinematicState {
    mycore::math::Vector2 position;
    mycore::math::Vector2 launch_velocity;

    bool operator==(const PlayerKinematicState&) const = default;
};

// Shared movement/launch integration used by the authoritative World and presentation-only
// extrapolation. The caller owns any additional causally dependent velocity, such as cohesion.
// normalized_movement must already satisfy is_valid_player_movement.
[[nodiscard]] mycore::math::Vector2
decayed_launch_velocity(mycore::math::Vector2 launch_velocity,
                        float decay_units_per_second_squared) noexcept;

[[nodiscard]] mycore::math::Vector2
player_kinematic_velocity(mycore::math::Vector2 normalized_movement,
                          mycore::math::Vector2 launch_velocity,
                          const WorldRules& rules) noexcept;

[[nodiscard]] PlayerKinematicState
advance_player_kinematics(PlayerKinematicState state,
                          mycore::math::Vector2 normalized_movement,
                          const WorldRules& rules,
                          mycore::math::Vector2 additional_velocity = {}) noexcept;

} // namespace dots::simulation
