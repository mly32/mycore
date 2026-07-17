#pragma once

#include "dots/simulation/ids.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/render_2d/render_2d.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace dots::presentation {

enum class CircleKind {
    Food,
    Player,
    PositionGhost,
};

struct CircleInstance {
    simulation::EntityId entity_id;
    mycore::math::Vector2 position;
    float mass{};
    float radius{};
    CircleKind kind{};

    auto operator<=>(const CircleInstance&) const = default;
};

struct FrameData {
    mycore::math::Vector2 camera;
    std::vector<CircleInstance> circles;
};

struct EntityPositionOverride {
    simulation::EntityId entity_id;
    mycore::math::Vector2 position;
};

struct InterpolatedFollowTarget {
    simulation::EntityId entity_id;
    mycore::math::Vector2 previous_position;
    mycore::math::Vector2 current_position;
    float alpha{};
    bool show_current_position_ghost{};
};

struct Settings {
    float pixels_per_world_unit{20.0F};
    bool draw_grid{true};
    float grid_spacing_world_units{8.0F};
    mycore::render::Color background{0.063F, 0.094F, 0.125F, 1.0F};
    mycore::render::Color grid{0.125F, 0.188F, 0.251F, 1.0F};
    mycore::render::Color player{0.298F, 0.788F, 0.941F, 1.0F};
    mycore::render::Color player_growth{1.0F, 0.820F, 0.4F, 1.0F};
    mycore::render::Color food{0.969F, 0.145F, 0.522F, 1.0F};
    mycore::render::Color comparison_ghost_outline{1.0F, 1.0F, 1.0F, 0.9F};
    float comparison_ghost_outline_pixels{2.0F};
};

[[nodiscard]] FrameData
extract_frame(const simulation::World& world,
              mycore::math::Vector2 camera,
              std::span<const EntityPositionOverride> position_overrides = {});

// Presentation only: the followed entity and camera must use the same interpolated sample.
// After rollback or teleport, reset the endpoints rather than blending from stale state.
[[nodiscard]] FrameData
extract_interpolated_follow_frame(const simulation::World& world,
                                  const InterpolatedFollowTarget& follow_target);

[[nodiscard]] mycore::render_2d::DrawList build_draw_list(const FrameData& frame,
                                                          const Settings& settings);

} // namespace dots::presentation
