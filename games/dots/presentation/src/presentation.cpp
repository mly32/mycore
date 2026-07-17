#include "dots/presentation/presentation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>

namespace dots::presentation {
namespace {

constexpr float kFoodForFullGrowthColor = 8.0F;

[[nodiscard]] mycore::render::Color
lerp(mycore::render::Color from, mycore::render::Color to, float amount) noexcept {
    return {
        .red = from.red + ((to.red - from.red) * amount),
        .green = from.green + ((to.green - from.green) * amount),
        .blue = from.blue + ((to.blue - from.blue) * amount),
        .alpha = from.alpha + ((to.alpha - from.alpha) * amount),
    };
}

[[nodiscard]] mycore::render::Color player_color(float mass, const Settings& settings) noexcept {
    const auto gained_food = (mass - simulation::kInitialPlayerMass) / simulation::kFoodMass;
    const auto growth = std::clamp(gained_food / kFoodForFullGrowthColor, 0.0F, 1.0F);
    return lerp(settings.player, settings.player_growth, growth);
}

} // namespace

FrameData extract_frame(const simulation::World& world,
                        mycore::math::Vector2 camera,
                        std::span<const EntityPositionOverride> position_overrides) {
    FrameData frame{.camera = camera, .circles = {}};
    frame.circles.reserve(world.food_count() + world.player_count());

    const auto append_entities = [&world, &frame, position_overrides](
                                     std::span<const simulation::EntityId> ids, CircleKind kind) {
        for (const auto entity_id : ids) {
            auto position = world.position(entity_id);
            const auto mass = world.mass(entity_id);
            const auto radius = world.radius(entity_id);
            const auto override = std::find_if(position_overrides.begin(),
                                               position_overrides.end(),
                                               [entity_id](const EntityPositionOverride& value) {
                                                   return value.entity_id == entity_id;
                                               });
            if (override != position_overrides.end()) {
                position = override->position;
            }
            if (!position || !mass || !radius || !std::isfinite(position->x) ||
                !std::isfinite(position->y) || !std::isfinite(*mass) || *mass <= 0.0F ||
                !std::isfinite(*radius) || *radius <= 0.0F) {
                throw std::runtime_error{"Dots presentation encountered invalid entity geometry"};
            }
            frame.circles.push_back({
                .entity_id = entity_id,
                .position = *position,
                .mass = *mass,
                .radius = *radius,
                .kind = kind,
            });
        }
    };

    append_entities(world.food_ids(), CircleKind::Food);
    append_entities(world.player_ids(), CircleKind::Player);
    return frame;
}

FrameData extract_interpolated_follow_frame(const simulation::World& world,
                                            const InterpolatedFollowTarget& follow_target) {
    if (!follow_target.entity_id.is_valid() || !world.contains(follow_target.entity_id) ||
        !std::isfinite(follow_target.previous_position.x) ||
        !std::isfinite(follow_target.previous_position.y) ||
        !std::isfinite(follow_target.current_position.x) ||
        !std::isfinite(follow_target.current_position.y) || !std::isfinite(follow_target.alpha) ||
        follow_target.alpha < 0.0F || follow_target.alpha > 1.0F) {
        throw std::runtime_error{"Dots presentation encountered invalid follow interpolation"};
    }

    const auto position =
        follow_target.previous_position +
        ((follow_target.current_position - follow_target.previous_position) * follow_target.alpha);
    const std::array overrides{
        EntityPositionOverride{
            .entity_id = follow_target.entity_id,
            .position = position,
        },
    };
    auto frame = extract_frame(world, position, overrides);
    if (follow_target.show_current_position_ghost) {
        const auto followed = std::find_if(frame.circles.begin(),
                                           frame.circles.end(),
                                           [&follow_target](const CircleInstance& circle) {
                                               return circle.entity_id == follow_target.entity_id;
                                           });
        if (followed == frame.circles.end()) {
            throw std::runtime_error{"Dots presentation could not find its follow target"};
        }
        auto ghost = *followed;
        ghost.position = follow_target.current_position;
        ghost.kind = CircleKind::PositionGhost;
        frame.circles.push_back(ghost);
    }
    return frame;
}

mycore::render_2d::DrawList build_draw_list(const FrameData& frame, const Settings& settings) {
    mycore::render_2d::DrawList draw_list{
        .camera =
            {
                .center = frame.camera,
                .pixels_per_world_unit = settings.pixels_per_world_unit,
            },
        .clear_color = settings.background,
        .grid = settings.draw_grid ? std::optional<mycore::render_2d::Grid>{{
                                         .spacing_world_units = settings.grid_spacing_world_units,
                                         .color = settings.grid,
                                     }}
                                   : std::nullopt,
        .circles = {},
    };
    draw_list.circles.reserve(frame.circles.size());
    for (const auto& circle : frame.circles) {
        if (circle.kind == CircleKind::PositionGhost) {
            auto transparent_fill = settings.comparison_ghost_outline;
            transparent_fill.alpha = 0.0F;
            draw_list.circles.push_back({
                .center = circle.position,
                .radius = circle.radius,
                .color = transparent_fill,
                .outline_color = settings.comparison_ghost_outline,
                .outline_width_pixels = settings.comparison_ghost_outline_pixels,
            });
            continue;
        }
        draw_list.circles.push_back({
            .center = circle.position,
            .radius = circle.radius,
            .color = circle.kind == CircleKind::Food ? settings.food
                                                     : player_color(circle.mass, settings),
            .outline_color = {},
            .outline_width_pixels = 0.0F,
        });
    }
    return draw_list;
}

} // namespace dots::presentation
