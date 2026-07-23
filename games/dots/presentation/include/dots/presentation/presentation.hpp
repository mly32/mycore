#pragma once

#include "dots/presentation/remote_presentation.hpp"
#include "dots/protocol/ids.hpp"
#include "dots/replication/replication.hpp"
#include "dots/simulation/ids.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/render_2d/render_2d.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dots::presentation {

enum class CircleKind : std::uint8_t {
    Food,
    Player,
    PositionGhost,
    PredictedPositionGhost,
    AuthoritativeSampleGhost,
    PreCorrectionGhost,
    ReplayMarker,
    RemoteOlderEndpointGhost,
    RemoteNewerEndpointGhost,
    RemoteInterpolationConnectorStart,
    RemoteInterpolationConnectorMiddle,
    RemoteInterpolationConnectorEnd,
};

struct CircleInstance {
    mycore::math::Vector2 position;
    float mass{};
    float radius{};
    CircleKind kind{};
    protocol::EntityId entity_id;

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

inline constexpr auto kPredictionCorrectionSmoothingDuration = std::chrono::milliseconds{100};
inline constexpr auto kPredictionDebugRetentionDuration = std::chrono::seconds{2};

struct LocalPredictionSample {
    mycore::math::Vector2 predicted_position;
    mycore::math::Vector2 accumulated_correction_displacement;
    std::uint64_t correction_sequence{};
    std::uint64_t hard_resync_sequence{};
    std::optional<mycore::math::Vector2> pre_correction_position;
    std::span<const mycore::math::Vector2> correction_replay_path;
};

class LocalPredictionPresentation {
public:
    void update(const LocalPredictionSample& sample, std::chrono::steady_clock::time_point now);
    void reset() noexcept;
    void clear_correction_visuals() noexcept;

    [[nodiscard]] mycore::math::Vector2 predicted_position() const noexcept;
    [[nodiscard]] mycore::math::Vector2 presentation_position() const noexcept;
    [[nodiscard]] mycore::math::Vector2 smoothing_offset() const noexcept;
    [[nodiscard]] bool correction_visual_active() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2>
    retained_pre_correction_position() const noexcept;
    [[nodiscard]] std::span<const mycore::math::Vector2>
    retained_correction_replay_path() const noexcept;

private:
    [[nodiscard]] mycore::math::Vector2
    evaluate_smoothing_offset(std::chrono::steady_clock::time_point now) const noexcept;
    void initialize(const LocalPredictionSample& sample,
                    std::chrono::steady_clock::time_point now) noexcept;

    mycore::math::Vector2 predicted_position_;
    mycore::math::Vector2 presentation_position_;
    mycore::math::Vector2 smoothing_offset_;
    mycore::math::Vector2 smoothing_start_offset_;
    mycore::math::Vector2 last_correction_accumulator_;
    std::optional<mycore::math::Vector2> retained_pre_correction_position_;
    std::vector<mycore::math::Vector2> retained_correction_replay_path_;
    std::chrono::steady_clock::time_point smoothing_start_time_;
    std::chrono::steady_clock::time_point correction_visual_expiry_;
    std::uint64_t last_correction_sequence_{};
    std::uint64_t last_hard_resync_sequence_{};
    bool initialized_{};
    bool smoothing_active_{};
    bool correction_visual_active_{};
};

struct PredictedReplicatedPlayer {
    protocol::EntityId entity_id;
    mycore::math::Vector2 presentation_position;
    mycore::math::Vector2 predicted_position;
    std::optional<mycore::math::Vector2> pre_correction_position;
    std::span<const mycore::math::Vector2> correction_replay_path;
    bool show_prediction_layers{true};
    bool show_replay_path{true};
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
    mycore::render::Color authoritative_sample_outline{1.0F, 0.55F, 0.12F, 0.95F};
    mycore::render::Color pre_correction_outline{1.0F, 0.1F, 0.75F, 0.95F};
    mycore::render::Color replay_marker{0.63F, 0.3F, 1.0F, 0.9F};
    mycore::render::Color remote_older_endpoint_outline{0.0F, 0.9F, 0.95F, 0.95F};
    mycore::render::Color remote_newer_endpoint_outline{0.2F, 0.45F, 1.0F, 0.95F};
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

[[nodiscard]] FrameData extract_replicated_frame(const replication::ReplicatedWorld& world,
                                                 protocol::EntityId controlled_entity_id);

[[nodiscard]] FrameData
extract_predicted_replicated_frame(const replication::ReplicatedWorld& world,
                                   const PredictedReplicatedPlayer& controlled_player);

[[nodiscard]] FrameData
extract_remote_interpolated_predicted_frame(const replication::ReplicatedWorld& world,
                                            const RemotePresentationFrame& remotes,
                                            std::span<const RemoteEntityEndpoints> remote_endpoints,
                                            const PredictedReplicatedPlayer& controlled_player);

[[nodiscard]] FrameData
extract_remote_interpolated_spectator_frame(const RemotePresentationFrame& remotes,
                                            std::span<const RemoteEntityEndpoints> remote_endpoints,
                                            mycore::math::Vector2 camera);

[[nodiscard]] mycore::render_2d::DrawList build_draw_list(const FrameData& frame,
                                                          const Settings& settings);

} // namespace dots::presentation
