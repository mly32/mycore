#pragma once

#include "dots/simulation/ids.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/time/time.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <vector>

namespace dots::simulation {

inline constexpr float kInitialPlayerMass = 16.0F;
inline constexpr float kFoodMass = 1.0F;
inline constexpr float kSpatialGridCellSize = 8.0F;
inline constexpr float kPlayerSpeedUnitsPerSecond = 6.0F;
inline constexpr std::uint32_t kSplitRecastTicks = 15;
inline constexpr std::uint32_t kMergeDelayTicks = 150;
inline constexpr std::uint16_t kMaximumPiecesPerOwner = 8;
inline constexpr float kMinimumSplitMass = 16.0F;
inline constexpr float kChildLaunchSpeedUnitsPerSecond = 18.0F;
inline constexpr float kLaunchDecayUnitsPerSecondSquared = 18.0F;
inline constexpr float kCohesionSpeedUnitsPerSecond = 3.0F;

// Rules are copied into a World and every complete checkpoint. The protocol will distribute the
// authoritative value in Feature 14's checkpoint integration.
struct WorldRules {
    float initial_player_mass{kInitialPlayerMass};
    float food_mass{kFoodMass};
    float spatial_grid_cell_size{kSpatialGridCellSize};
    float player_speed_units_per_second{kPlayerSpeedUnitsPerSecond};
    std::uint32_t split_recast_ticks{kSplitRecastTicks};
    std::uint32_t merge_delay_ticks{kMergeDelayTicks};
    std::uint16_t maximum_pieces_per_owner{kMaximumPiecesPerOwner};
    float minimum_split_mass{kMinimumSplitMass};
    float child_launch_speed_units_per_second{kChildLaunchSpeedUnitsPerSecond};
    float launch_decay_units_per_second_squared{kLaunchDecayUnitsPerSecondSquared};
    float cohesion_speed_units_per_second{kCohesionSpeedUnitsPerSecond};

    bool operator==(const WorldRules&) const = default;
};

struct PredictionKey {
    PlayerOwnerId owner_id;
    InputCommandId input_id;
    std::uint16_t child_ordinal{};

    auto operator<=>(const PredictionKey&) const = default;
};

struct OwnerCheckpoint {
    PlayerOwnerId owner_id;
    std::vector<EntityId> player_ids;
    mycore::math::Vector2 movement;
    mycore::math::Vector2 last_non_zero_movement;
    InputCommandId last_input_id;
    mycore::time::Tick split_cooldown_end_tick;

    bool operator==(const OwnerCheckpoint&) const = default;
};

struct PlayerCheckpoint {
    EntityId entity_id;
    PlayerOwnerId owner_id;
    mycore::math::Vector2 position;
    float mass{};
    mycore::math::Vector2 launch_velocity;
    mycore::time::Tick merge_eligible_tick;
    std::optional<PredictionKey> prediction_key;

    bool operator==(const PlayerCheckpoint&) const = default;
};

struct FoodCheckpoint {
    EntityId entity_id;
    mycore::math::Vector2 position;

    bool operator==(const FoodCheckpoint&) const = default;
};

// Complete, owning replay state in canonical owner/entity order. Derived radius, lookup, and
// spatial-grid storage are intentionally rebuilt during restore.
struct WorldCheckpoint {
    WorldRules rules;
    mycore::time::Tick tick;
    std::uint32_t next_entity_id{};
    std::vector<OwnerCheckpoint> owners;
    std::vector<PlayerCheckpoint> players;
    std::vector<FoodCheckpoint> food;

    bool operator==(const WorldCheckpoint&) const = default;
};

enum class CheckpointRestoreError : std::uint8_t {
    InvalidRules,
    InvalidOrdering,
    InvalidOwnerState,
    InvalidEntityState,
    InvalidGeometry,
};

} // namespace dots::simulation
