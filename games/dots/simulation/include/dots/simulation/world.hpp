#pragma once

#include "dots/simulation/ids.hpp"
#include "dots/simulation/input_command.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/time/time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dots::simulation {

inline constexpr std::uint32_t kTickRateHz = 30;
inline constexpr auto kTickDuration =
    std::chrono::nanoseconds{std::chrono::seconds{1}} / kTickRateHz;
inline constexpr float kPlayerSpeedUnitsPerSecond = 6.0F;

// A fixed-step world using an explicit structure-of-arrays (SoA) data model: each player
// component has a dense parallel array, and matching indices associate components with an ID.
// SoA suits Dots while most entities share one schema and systems process one component in bulk.
// Prefer an ECS when many entity kinds need changing component combinations, or an
// array-of-structures model when most logic repeatedly accesses every field of one entity.
class World {
public:
    [[nodiscard]] EntityId spawn_player(mycore::math::Vector2 position = {});
    [[nodiscard]] bool remove_player(EntityId entity_id);

    [[nodiscard]] bool contains(EntityId entity_id) const noexcept;
    [[nodiscard]] std::size_t player_count() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> position(EntityId entity_id) const noexcept;
    [[nodiscard]] mycore::time::Tick tick() const noexcept;

    [[nodiscard]] bool apply_input(const InputCommand& command);
    void step() noexcept;

private:
    [[nodiscard]] std::optional<std::size_t> find_index(EntityId entity_id) const noexcept;

    std::vector<EntityId> entity_ids_;
    std::vector<mycore::math::Vector2> positions_;
    std::vector<mycore::math::Vector2> movements_;
    std::vector<InputCommandId> last_input_ids_;
    std::uint32_t next_entity_id_{};
    mycore::time::Tick tick_{};
};

} // namespace dots::simulation
