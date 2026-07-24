#pragma once

#include "dots/presentation/remote_presentation.hpp"
#include "dots/protocol/ids.hpp"
#include "mycore/math/vector2.hpp"

#include <cstdint>

namespace dots::presentation {

enum class SpectatorCameraMode : std::uint8_t {
    FollowKiller,
    Free,
};

struct SpectatorCameraSettings {
    float pan_speed_world_units_per_second{12.0F};
    float minimum_pixels_per_world_unit{5.0F};
    float maximum_pixels_per_world_unit{80.0F};
};

struct SpectatorCameraInput {
    mycore::math::Vector2 pan;
    int zoom_steps{};
    bool toggle_follow{};
};

class SpectatorCamera {
public:
    explicit SpectatorCamera(SpectatorCameraSettings settings = {});

    void enter(mycore::math::Vector2 initial_position,
               float initial_pixels_per_world_unit,
               protocol::EntityId confirmed_follow_entity_id,
               const RemotePresentationFrame& frame);
    void update(const RemotePresentationFrame& frame,
                protocol::EntityId confirmed_follow_entity_id,
                SpectatorCameraInput input,
                float elapsed_seconds);

    [[nodiscard]] SpectatorCameraMode mode() const noexcept;
    [[nodiscard]] mycore::math::Vector2 position() const noexcept;
    [[nodiscard]] float pixels_per_world_unit() const noexcept;

private:
    [[nodiscard]] static const RemoteEntitySample*
    find_confirmed_follow(const RemotePresentationFrame& frame,
                          protocol::EntityId confirmed_follow_entity_id) noexcept;

    SpectatorCameraSettings settings_;
    SpectatorCameraMode mode_{SpectatorCameraMode::Free};
    mycore::math::Vector2 position_;
    float pixels_per_world_unit_{20.0F};
    bool follow_sample_seen_{};
};

} // namespace dots::presentation
