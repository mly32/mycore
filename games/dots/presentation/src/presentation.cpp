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
constexpr float kPredictedOutlineRadiusOffsetPixels = 1.0F;
constexpr float kAuthoritativeOutlineRadiusOffsetPixels = 2.0F;
constexpr float kPreCorrectionOutlineRadiusOffsetPixels = 3.0F;
constexpr float kReplayMarkerRadiusPixels = 3.0F;

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

[[nodiscard]] bool finite(mycore::math::Vector2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

void append_outline(mycore::render_2d::DrawList& draw_list,
                    const CircleInstance& circle,
                    mycore::render::Color color,
                    float radius_offset_pixels,
                    const Settings& settings) {
    auto transparent_fill = color;
    transparent_fill.alpha = 0.0F;
    draw_list.circles.push_back({
        .center = circle.position,
        .radius = circle.radius + (radius_offset_pixels / settings.pixels_per_world_unit),
        .color = transparent_fill,
        .outline_color = color,
        .outline_width_pixels = settings.comparison_ghost_outline_pixels,
    });
}

} // namespace

void LocalPredictionPresentation::update(const LocalPredictionSample& sample,
                                         std::chrono::steady_clock::time_point now) {
    if (!finite(sample.predicted_position) || !finite(sample.accumulated_correction_displacement) ||
        (sample.pre_correction_position && !finite(*sample.pre_correction_position)) ||
        std::any_of(sample.correction_replay_path.begin(),
                    sample.correction_replay_path.end(),
                    [](mycore::math::Vector2 position) {
                        return !finite(position);
                    })) {
        throw std::runtime_error{"Dots prediction presentation encountered invalid state"};
    }

    if (!initialized_ || sample.hard_resync_sequence < last_hard_resync_sequence_ ||
        (sample.hard_resync_sequence == last_hard_resync_sequence_ &&
         sample.correction_sequence < last_correction_sequence_)) {
        initialize(sample, now);
        return;
    }

    auto residual = evaluate_smoothing_offset(now);
    const auto hard_resynced = sample.hard_resync_sequence != last_hard_resync_sequence_;
    if (hard_resynced) {
        residual = {};
        smoothing_start_offset_ = {};
        smoothing_start_time_ = now;
        smoothing_active_ = false;
        last_correction_accumulator_ = {};
        last_correction_sequence_ = 0;
        clear_correction_visuals();
    }

    if (sample.correction_sequence != last_correction_sequence_) {
        const auto correction_displacement =
            sample.accumulated_correction_displacement - last_correction_accumulator_;
        smoothing_start_offset_ = residual + correction_displacement;
        smoothing_start_time_ = now;
        smoothing_active_ = smoothing_start_offset_ != mycore::math::Vector2{};
        retained_pre_correction_position_ = sample.pre_correction_position;
        retained_correction_replay_path_.assign(sample.correction_replay_path.begin(),
                                                sample.correction_replay_path.end());
        correction_visual_expiry_ = now + kPredictionDebugRetentionDuration;
        correction_visual_active_ = true;
    }

    last_correction_sequence_ = sample.correction_sequence;
    last_hard_resync_sequence_ = sample.hard_resync_sequence;
    last_correction_accumulator_ = sample.accumulated_correction_displacement;
    predicted_position_ = sample.predicted_position;
    smoothing_offset_ = evaluate_smoothing_offset(now);
    presentation_position_ = predicted_position_ + smoothing_offset_;
    if (correction_visual_active_ && now >= correction_visual_expiry_) {
        clear_correction_visuals();
    }
}

void LocalPredictionPresentation::clear_correction_visuals() noexcept {
    correction_visual_active_ = false;
    retained_pre_correction_position_.reset();
    retained_correction_replay_path_.clear();
}

mycore::math::Vector2 LocalPredictionPresentation::predicted_position() const noexcept {
    return predicted_position_;
}

mycore::math::Vector2 LocalPredictionPresentation::presentation_position() const noexcept {
    return presentation_position_;
}

mycore::math::Vector2 LocalPredictionPresentation::smoothing_offset() const noexcept {
    return smoothing_offset_;
}

bool LocalPredictionPresentation::correction_visual_active() const noexcept {
    return correction_visual_active_;
}

std::optional<mycore::math::Vector2>
LocalPredictionPresentation::retained_pre_correction_position() const noexcept {
    return retained_pre_correction_position_;
}

std::span<const mycore::math::Vector2>
LocalPredictionPresentation::retained_correction_replay_path() const noexcept {
    return {retained_correction_replay_path_.data(), retained_correction_replay_path_.size()};
}

mycore::math::Vector2 LocalPredictionPresentation::evaluate_smoothing_offset(
    std::chrono::steady_clock::time_point now) const noexcept {
    if (!smoothing_active_) {
        return {};
    }
    const auto elapsed =
        std::max(now - smoothing_start_time_, std::chrono::steady_clock::duration::zero());
    const auto progress =
        std::chrono::duration<float>{elapsed}.count() /
        std::chrono::duration<float>{kPredictionCorrectionSmoothingDuration}.count();
    return smoothing_start_offset_ * std::clamp(1.0F - progress, 0.0F, 1.0F);
}

void LocalPredictionPresentation::initialize(const LocalPredictionSample& sample,
                                             std::chrono::steady_clock::time_point now) noexcept {
    predicted_position_ = sample.predicted_position;
    presentation_position_ = sample.predicted_position;
    smoothing_offset_ = {};
    smoothing_start_offset_ = {};
    last_correction_accumulator_ = sample.accumulated_correction_displacement;
    smoothing_start_time_ = now;
    last_correction_sequence_ = sample.correction_sequence;
    last_hard_resync_sequence_ = sample.hard_resync_sequence;
    initialized_ = true;
    smoothing_active_ = false;
    clear_correction_visuals();
}

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
        const auto mass = world.mass(follow_target.entity_id);
        const auto radius = world.radius(follow_target.entity_id);
        if (!mass || !radius) {
            throw std::runtime_error{"Dots presentation could not find its follow target"};
        }
        frame.circles.push_back({
            .position = follow_target.current_position,
            .mass = *mass,
            .radius = *radius,
            .kind = CircleKind::PositionGhost,
        });
    }
    return frame;
}

FrameData extract_replicated_frame(const replication::ReplicatedWorld& world,
                                   protocol::EntityId controlled_entity_id) {
    const auto* controlled = world.find(controlled_entity_id);
    if (controlled == nullptr || controlled->kind != protocol::EntityKind::Player) {
        throw std::runtime_error{"Dots replicated presentation could not find its player"};
    }

    FrameData frame{.camera = {controlled->position_x, controlled->position_y}, .circles = {}};
    frame.circles.reserve(world.entities().size());
    for (const auto& entity : world.entities()) {
        if (!std::isfinite(entity.position_x) || !std::isfinite(entity.position_y) ||
            !std::isfinite(entity.mass) || entity.mass <= 0.0F) {
            throw std::runtime_error{"Dots replicated presentation encountered invalid geometry"};
        }
        frame.circles.push_back({
            .position = {entity.position_x, entity.position_y},
            .mass = entity.mass,
            .radius = simulation::radius_for_mass(entity.mass),
            .kind =
                entity.kind == protocol::EntityKind::Food ? CircleKind::Food : CircleKind::Player,
        });
    }
    return frame;
}

FrameData extract_predicted_replicated_frame(const replication::ReplicatedWorld& world,
                                             const PredictedReplicatedPlayer& controlled_player) {
    if (!finite(controlled_player.presentation_position) ||
        !finite(controlled_player.predicted_position) ||
        (controlled_player.pre_correction_position &&
         !finite(*controlled_player.pre_correction_position))) {
        throw std::runtime_error{"Dots predicted presentation encountered invalid geometry"};
    }
    const auto* controlled = world.find(controlled_player.entity_id);
    if (controlled == nullptr || controlled->kind != protocol::EntityKind::Player) {
        throw std::runtime_error{"Dots predicted presentation could not find its player"};
    }

    auto frame = extract_replicated_frame(world, controlled_player.entity_id);
    const auto controlled_iterator =
        std::find_if(world.entities().begin(),
                     world.entities().end(),
                     [&controlled_player](const protocol::EntityState& entity) {
                         return entity.entity_id == controlled_player.entity_id;
                     });
    if (controlled_iterator == world.entities().end()) {
        throw std::runtime_error{"Dots predicted presentation lost its player"};
    }
    const auto controlled_index =
        static_cast<std::size_t>(std::distance(world.entities().begin(), controlled_iterator));
    frame.camera = controlled_player.presentation_position;
    frame.circles[controlled_index].position = controlled_player.presentation_position;

    const auto radius = simulation::radius_for_mass(controlled->mass);
    const auto append_ghost = [&frame, controlled, radius](mycore::math::Vector2 position,
                                                           CircleKind kind) {
        frame.circles.push_back({
            .position = position,
            .mass = controlled->mass,
            .radius = radius,
            .kind = kind,
        });
    };
    if (controlled_player.show_prediction_layers) {
        append_ghost(controlled_player.predicted_position, CircleKind::PredictedPositionGhost);
        append_ghost({controlled->position_x, controlled->position_y},
                     CircleKind::AuthoritativeSampleGhost);
        if (controlled_player.pre_correction_position) {
            append_ghost(*controlled_player.pre_correction_position,
                         CircleKind::PreCorrectionGhost);
        }
    }
    if (controlled_player.show_replay_path) {
        for (const auto position : controlled_player.correction_replay_path) {
            if (!finite(position)) {
                throw std::runtime_error{"Dots replay visualization encountered invalid geometry"};
            }
            append_ghost(position, CircleKind::ReplayMarker);
        }
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
            append_outline(draw_list, circle, settings.comparison_ghost_outline, 0.0F, settings);
            continue;
        }
        if (circle.kind == CircleKind::PredictedPositionGhost) {
            append_outline(draw_list,
                           circle,
                           settings.comparison_ghost_outline,
                           kPredictedOutlineRadiusOffsetPixels,
                           settings);
            continue;
        }
        if (circle.kind == CircleKind::AuthoritativeSampleGhost) {
            append_outline(draw_list,
                           circle,
                           settings.authoritative_sample_outline,
                           kAuthoritativeOutlineRadiusOffsetPixels,
                           settings);
            continue;
        }
        if (circle.kind == CircleKind::PreCorrectionGhost) {
            append_outline(draw_list,
                           circle,
                           settings.pre_correction_outline,
                           kPreCorrectionOutlineRadiusOffsetPixels,
                           settings);
            continue;
        }
        if (circle.kind == CircleKind::ReplayMarker) {
            draw_list.circles.push_back({
                .center = circle.position,
                .radius = kReplayMarkerRadiusPixels / settings.pixels_per_world_unit,
                .color = settings.replay_marker,
                .outline_color = {},
                .outline_width_pixels = 0.0F,
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
