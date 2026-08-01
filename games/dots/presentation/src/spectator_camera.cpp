#include "dots/presentation/spectator_camera.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dots::presentation {
namespace {

constexpr float kZoomStepMultiplier = 1.1F;
constexpr int kMaximumZoomStepsPerUpdate = 64;

[[nodiscard]] bool finite(mycore::math::Vector2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

} // namespace

SpectatorCamera::SpectatorCamera(SpectatorCameraSettings settings)
    : settings_(settings) {
    if (!std::isfinite(settings_.pan_speed_world_units_per_second) ||
        settings_.pan_speed_world_units_per_second <= 0.0F ||
        !std::isfinite(settings_.minimum_pixels_per_world_unit) ||
        settings_.minimum_pixels_per_world_unit <= 0.0F ||
        !std::isfinite(settings_.maximum_pixels_per_world_unit) ||
        settings_.maximum_pixels_per_world_unit < settings_.minimum_pixels_per_world_unit) {
        throw std::invalid_argument{"Dots spectator camera settings are invalid"};
    }
}

void SpectatorCamera::enter(mycore::math::Vector2 initial_position,
                            float initial_pixels_per_world_unit,
                            protocol::EntityId confirmed_follow_entity_id,
                            const FrameData& frame) {
    if (!finite(initial_position) || !std::isfinite(initial_pixels_per_world_unit) ||
        initial_pixels_per_world_unit <= 0.0F) {
        throw std::invalid_argument{"Dots spectator camera initial state is invalid"};
    }
    position_ = initial_position;
    pixels_per_world_unit_ = std::clamp(initial_pixels_per_world_unit,
                                        settings_.minimum_pixels_per_world_unit,
                                        settings_.maximum_pixels_per_world_unit);
    mode_ = SpectatorCameraMode::Free;
    follow_sample_seen_ = false;
    if (const auto* follow = find_confirmed_follow(frame, confirmed_follow_entity_id)) {
        mode_ = SpectatorCameraMode::FollowKiller;
        position_ = follow->position;
        follow_sample_seen_ = true;
    } else if (confirmed_follow_entity_id.is_valid()) {
        mode_ = SpectatorCameraMode::FollowKiller;
    }
}

void SpectatorCamera::update(const FrameData& frame,
                             protocol::EntityId confirmed_follow_entity_id,
                             SpectatorCameraInput input,
                             float elapsed_seconds) {
    if (!finite(input.pan) || !std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0F) {
        throw std::invalid_argument{"Dots spectator camera input is invalid"};
    }

    const auto zoom_steps =
        std::clamp(input.zoom_steps, -kMaximumZoomStepsPerUpdate, kMaximumZoomStepsPerUpdate);
    pixels_per_world_unit_ = std::clamp(
        pixels_per_world_unit_ * std::pow(kZoomStepMultiplier, static_cast<float>(zoom_steps)),
        settings_.minimum_pixels_per_world_unit,
        settings_.maximum_pixels_per_world_unit);

    const auto* follow = find_confirmed_follow(frame, confirmed_follow_entity_id);
    if (mode_ == SpectatorCameraMode::FollowKiller) {
        if (follow == nullptr && (follow_sample_seen_ || !confirmed_follow_entity_id.is_valid())) {
            mode_ = SpectatorCameraMode::Free;
        } else if (follow != nullptr) {
            position_ = follow->position;
            follow_sample_seen_ = true;
        }
    }

    if (input.toggle_follow) {
        if (mode_ == SpectatorCameraMode::FollowKiller) {
            mode_ = SpectatorCameraMode::Free;
        } else if (follow != nullptr) {
            mode_ = SpectatorCameraMode::FollowKiller;
            position_ = follow->position;
            follow_sample_seen_ = true;
        }
    }

    if (mode_ == SpectatorCameraMode::Free) {
        position_ += input.pan * (settings_.pan_speed_world_units_per_second * elapsed_seconds);
    } else if (follow != nullptr) {
        position_ = follow->position;
    }
}

SpectatorCameraMode SpectatorCamera::mode() const noexcept {
    return mode_;
}

mycore::math::Vector2 SpectatorCamera::position() const noexcept {
    return position_;
}

float SpectatorCamera::pixels_per_world_unit() const noexcept {
    return pixels_per_world_unit_;
}

const CircleInstance*
SpectatorCamera::find_confirmed_follow(const FrameData& frame,
                                       protocol::EntityId confirmed_follow_entity_id) noexcept {
    if (!confirmed_follow_entity_id.is_valid()) {
        return nullptr;
    }
    const auto iterator = std::find_if(frame.circles.begin(),
                                       frame.circles.end(),
                                       [confirmed_follow_entity_id](const CircleInstance& circle) {
                                           return circle.entity_id == confirmed_follow_entity_id &&
                                                  circle.kind == CircleKind::Player &&
                                                  finite(circle.position);
                                       });
    return iterator == frame.circles.end() ? nullptr : &*iterator;
}

} // namespace dots::presentation
