#pragma once

#include "dots/simulation/ids.hpp"
#include "dots/simulation/input_command.hpp"
#include "dots/simulation/movement.hpp"
#include "dots/simulation/spatial_grid.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/time/time.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dots::simulation {

inline constexpr float kInitialPlayerMass = 16.0F;
inline constexpr float kFoodMass = 1.0F;
inline constexpr float kSpatialGridCellSize = 8.0F;

[[nodiscard]] inline float radius_for_mass(float mass) noexcept {
    return std::sqrt(mass);
}

// A fixed-step world using an explicit structure-of-arrays (SoA) data model: each player
// component has a dense parallel array, and matching indices associate components with an ID.
// SoA suits Dots while most entities share one schema and systems process one component in bulk.
// Prefer an ECS when many entity kinds need changing component combinations, or an
// array-of-structures model when most logic repeatedly accesses every field of one entity.
class World {
public:
    [[nodiscard]] std::optional<EntityId> spawn_player(mycore::math::Vector2 position = {});
    [[nodiscard]] bool remove_player(EntityId entity_id);
    [[nodiscard]] std::optional<EntityId> spawn_food(mycore::math::Vector2 position);
    [[nodiscard]] bool remove_food(EntityId entity_id);

    [[nodiscard]] bool contains(EntityId entity_id) const noexcept;
    [[nodiscard]] std::size_t player_count() const noexcept;
    [[nodiscard]] std::size_t food_count() const noexcept;
    [[nodiscard]] std::size_t occupied_spatial_cell_count() const noexcept;
    [[nodiscard]] std::span<const EntityId> player_ids() const noexcept;
    [[nodiscard]] std::span<const EntityId> food_ids() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> position(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<float> mass(EntityId entity_id) const noexcept;
    [[nodiscard]] std::optional<float> radius(EntityId entity_id) const noexcept;
    [[nodiscard]] mycore::time::Tick tick() const noexcept;

    [[nodiscard]] bool apply_input(const InputCommand& command);
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
    [[nodiscard]] std::optional<EntityId> next_entity_id() const noexcept;
    void reserve_player_capacity();
    void reserve_food_capacity();
    void resolve_food_collisions();

    std::vector<EntityId> entity_ids_;
    std::vector<mycore::math::Vector2> positions_;
    std::vector<mycore::math::Vector2> movements_;
    std::vector<InputCommandId> last_input_ids_;
    std::vector<float> masses_;
    std::vector<float> radii_;
    std::vector<EntityId> food_entity_ids_;
    std::vector<mycore::math::Vector2> food_positions_;
    std::vector<std::optional<EntityLocation>> entity_locations_;
    SpatialGrid spatial_grid_{kSpatialGridCellSize};
    std::uint32_t next_entity_id_{};
    mycore::time::Tick tick_{};
};

} // namespace dots::simulation
