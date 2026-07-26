#pragma once

#include "dots/simulation/ids.hpp"
#include "dots/simulation/movement.hpp"
#include "dots/simulation/spatial_grid.hpp"
#include "dots/simulation/tick.hpp"
#include "dots/simulation/world_state.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/time/time.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace dots::simulation {

[[nodiscard]] inline float radius_for_mass(float mass) noexcept {
    return std::sqrt(mass);
}

enum class InitialPlayerSpawnStatus : std::uint8_t {
    Clear,
    Blocked,
    OutsideRepresentableGrid,
};

// A fixed-step world using an explicit structure-of-arrays (SoA) data model: each player
// component has a dense parallel array, and matching indices associate components with an ID.
// SoA suits Dots while most entities share one schema and systems process one component in bulk.
// Prefer an ECS when many entity kinds need changing component combinations, or an
// array-of-structures model when most logic repeatedly accesses every field of one entity.
class World {
public:
    World();
    explicit World(WorldRules rules);

    [[nodiscard]] std::optional<EntityId>
    spawn_player(PlayerOwnerId owner_id,
                 mycore::math::Vector2 position = {},
                 std::optional<PredictionKey> prediction_key = std::nullopt);
    [[nodiscard]] bool remove_player(EntityId entity_id);
    [[nodiscard]] std::optional<EntityId> spawn_food(mycore::math::Vector2 position);
    [[nodiscard]] bool remove_food(EntityId entity_id);

    [[nodiscard]] const WorldRules& rules() const noexcept;
    [[nodiscard]] bool contains(EntityId entity_id) const noexcept;
    [[nodiscard]] std::size_t player_count() const noexcept;
    [[nodiscard]] std::size_t food_count() const noexcept;
    [[nodiscard]] std::size_t occupied_spatial_cell_count() const noexcept;
    [[nodiscard]] std::span<const EntityId> player_ids() const noexcept;
    [[nodiscard]] std::span<const EntityId> food_ids() const noexcept;
    [[nodiscard]] std::optional<PlayerOwnerId> player_owner(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> position(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<float> mass(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<float> radius(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<PredictionKey> prediction_key(EntityId entity_id) const noexcept;
    [[nodiscard]] bool has_available_entity_id() const noexcept;
    [[nodiscard]] InitialPlayerSpawnStatus
    classify_initial_player_spawn(mycore::math::Vector2 position) const;
    [[nodiscard]] mycore::time::Tick tick() const noexcept;
    [[nodiscard]] const TickJournal& last_tick_journal() const noexcept;
    [[nodiscard]] WorldCheckpoint checkpoint() const;
    // Validates and builds a scratch World before publishing; an error leaves this World intact.
    [[nodiscard]] std::optional<CheckpointRestoreError> restore(const WorldCheckpoint& checkpoint);

    // Installs the owner commands, advances one fixed tick, and publishes World plus journal as
    // one transaction. A TickError leaves the previous checkpoint and journal intact.
    [[nodiscard]] TickResult advance(std::span<const TickCommand> commands);
    [[nodiscard]] TickResult advance(const TickCommand& command);
    // Convenience for a tick with no newly sampled commands; held owner movement still applies.
    [[nodiscard]] bool step();

private:
    enum class EntityKind : std::uint8_t {
        Player,
        Food,
    };

    struct EntityLocation {
        EntityKind kind;
        std::size_t index;
    };

    [[nodiscard]] const EntityLocation* find_location(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_index(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_food_index(EntityId entity_id) const noexcept;
    [[nodiscard]] const OwnerCheckpoint* find_owner(PlayerOwnerId owner_id) const noexcept;
    [[nodiscard]] OwnerCheckpoint* find_owner(PlayerOwnerId owner_id) noexcept;
    [[nodiscard]] std::optional<EntityId> next_entity_id() const noexcept;
    [[nodiscard]] std::optional<TickError>
    apply_commands(std::span<const TickCommand> commands,
                   std::vector<OwnerCheckpoint>& next_owners) const;
    [[nodiscard]] bool advance_simulation(std::vector<OwnerCheckpoint> next_owners,
                                          TickJournal& journal);
    void reserve_player_capacity();
    void reserve_food_capacity();

    WorldRules rules_;
    std::vector<OwnerCheckpoint> owners_;
    std::vector<EntityId> entity_ids_;
    std::vector<PlayerOwnerId> owner_ids_;
    std::vector<mycore::math::Vector2> positions_;
    std::vector<float> masses_;
    std::vector<float> radii_;
    std::vector<mycore::math::Vector2> launch_velocities_;
    std::vector<mycore::time::Tick> merge_eligible_ticks_;
    std::vector<std::optional<PredictionKey>> prediction_keys_;
    std::vector<EntityId> food_entity_ids_;
    std::vector<mycore::math::Vector2> food_positions_;
    std::unordered_map<std::uint32_t, EntityLocation> entity_locations_;
    SpatialGrid spatial_grid_;
    std::uint32_t next_entity_id_{};
    mycore::time::Tick tick_{};
    TickJournal last_tick_journal_;
};

} // namespace dots::simulation
