#include "dots/presentation/presentation.hpp"

#include <algorithm>
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

FrameData extract_frame(const simulation::World& world, mycore::math::Vector2 camera) {
    FrameData frame{.camera = camera};
    frame.circles.reserve(world.food_count() + world.player_count());

    const auto append_entities = [&world, &frame](std::span<const simulation::EntityId> ids,
                                                  CircleKind kind) {
        for (const auto entity_id : ids) {
            const auto position = world.position(entity_id);
            const auto mass = world.mass(entity_id);
            const auto radius = world.radius(entity_id);
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
    };
    draw_list.circles.reserve(frame.circles.size());
    for (const auto& circle : frame.circles) {
        draw_list.circles.push_back({
            .center = circle.position,
            .radius = circle.radius,
            .color = circle.kind == CircleKind::Food ? settings.food
                                                     : player_color(circle.mass, settings),
        });
    }
    return draw_list;
}

} // namespace dots::presentation
