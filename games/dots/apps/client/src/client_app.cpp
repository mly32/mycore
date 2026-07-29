#include "dots/client/client_app.hpp"

#include "dots/client/controls.hpp"
#include "dots/client_runtime/client_runtime.hpp"
#include "dots/prediction/prediction.hpp"
#include "dots/presentation/presentation.hpp"
#include "dots/presentation/rollback_consequences.hpp"
#include "dots/presentation/spectator_camera.hpp"
#include "dots/protocol/codec.hpp"
#include "dots/server/server_runtime.hpp"
#include "dots/simulation/world.hpp"
#include "dots/simulation/world_setup.hpp"
#include "mycore/assets/directory_source.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/debug/metrics.hpp"
#include "mycore/debug/profile.hpp"
#include "mycore/debug_ui/context.hpp"
#include "mycore/debug_ui/widgets.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/net_transport/net_transport.hpp"
#include "mycore/platform_sdl/input.hpp"
#include "mycore/platform_sdl/runtime.hpp"
#include "mycore/platform_sdl/window.hpp"
#include "mycore/render/render.hpp"
#include "mycore/render_2d/render_2d.hpp"
#include "mycore/time/time.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <imgui.h>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dots::client {
namespace {

using mycore::math::Vector2;

mycore::platform_sdl::WindowFlags window_flags(const WindowSettings& settings) {
    using mycore::platform_sdl::WindowFlags;
    auto flags = WindowFlags::None;
    if (settings.resizable) {
        flags = flags | WindowFlags::Resizable;
    }
    if (settings.fullscreen) {
        flags = flags | WindowFlags::Fullscreen;
    }
    if (settings.high_dpi) {
        flags = flags | WindowFlags::HighPixelDensity;
    }
    return flags;
}

[[nodiscard]] mycore::render::Color to_render_color(RgbColor color) noexcept {
    constexpr float kColorScale = 1.0F / 255.0F;
    return {
        static_cast<float>(color.red) * kColorScale,
        static_cast<float>(color.green) * kColorScale,
        static_cast<float>(color.blue) * kColorScale,
        1.0F,
    };
}

[[nodiscard]] dots::presentation::Settings presentation_settings(const ClientConfig& config) {
    return {
        .pixels_per_world_unit = config.view.pixels_per_world_unit,
        .draw_grid = config.view.draw_grid,
        .grid_spacing_world_units = config.view.grid_spacing_world_units,
        .background = to_render_color(config.colors.background),
        .grid = to_render_color(config.colors.grid),
        .player = to_render_color(config.colors.player),
        .player_growth = to_render_color(config.colors.player_growth),
        .food = to_render_color(config.colors.food),
    };
}

[[nodiscard]] constexpr bool gpu_debug_mode() noexcept {
#if defined(NDEBUG)
    return false;
#else
    return true;
#endif
}

struct DebugWorldStats {
    std::string_view presentation;
    std::uint64_t tick{};
    std::size_t player_count{};
    std::size_t food_count{};
    std::optional<Vector2> current_player_position;
    std::optional<std::size_t> occupied_grid_cells;
    std::optional<std::uint32_t> snapshot_id;
    std::optional<mycore::net_transport::TransportStatistics> transport;
    std::optional<dots::client_runtime::ReplicationStatistics> replication;
    struct NetworkSession {
        dots::client_runtime::State runtime_state{};
        dots::protocol::ClientId client_id;
        dots::protocol::EntityId controlled_entity_id;
        bool local_prediction_available{};
        mycore::net_transport::ConnectionHandle connection_handle;
        std::uint32_t server_tick{};
        std::uint32_t local_input_tick{};
        dots::client_runtime::PredictionStatistics prediction;
        dots::presentation::RemotePresentationStatistics remote_presentation;
        dots::presentation::RemoteExtrapolationStatistics remote_extrapolation;
        dots::presentation::ConsequencePresentationStatistics consequences;
        dots::presentation::PersistentPresentationStatistics persistent_presentation;
        RemotePresentationMode remote_presentation_mode{RemotePresentationMode::Extrapolated};
        std::optional<dots::protocol::EntityId> representative_remote_entity;
        dots::presentation::RemoteEntityEndpoints representative_remote_endpoints;
        std::optional<Vector2> latest_authoritative_sample;
        std::optional<Vector2> predicted_position;
        std::optional<Vector2> presentation_position;
        std::optional<Vector2> smoothing_offset;
        std::optional<Vector2> last_nonzero_movement_input;
        std::size_t retained_correction_count{};
        std::size_t correction_history_capacity{};
        std::size_t retained_local_correction_count{};
        std::size_t retained_remote_correction_count{};
    };
    std::optional<NetworkSession> network_session;
    struct GameplaySession {
        dots::protocol::ClientId client_id;
        dots::protocol::SessionMode mode{dots::protocol::SessionMode::Playing};
        std::size_t owned_piece_count{};
        dots::protocol::EntityId primary_entity_id;
        dots::protocol::EntityId follow_entity_id;
        std::optional<std::uint32_t> defeat_tick;
        std::optional<std::uint32_t> respawn_available_tick;
        std::optional<double> respawn_seconds_remaining;
        std::optional<dots::protocol::PlayerAbsorbed> latest_absorption;
        dots::protocol::InputSequenceId latest_respawn_request_id;
        dots::protocol::RespawnResult latest_respawn_result{dots::protocol::RespawnResult::None};
    };
    std::optional<GameplaySession> gameplay_session;
};

constexpr std::size_t kInjectedInputDropBurstSize = 3;
constexpr auto kFaultCompletionReceiptDuration = std::chrono::seconds{2};

struct PredictionDebugControls {
    bool show_prediction_layers{true};
    bool show_replay_path{true};
    bool show_remote_endpoint_layers{true};
    std::optional<Vector2> requested_prediction_error;
    bool drop_input_packets_requested{};
    bool clear_correction_visuals_requested{};
    std::optional<std::uint64_t> input_drop_count_at_burst_start;
    std::optional<std::chrono::steady_clock::time_point> input_drop_burst_completed_at;

    void begin_input_drop_burst(std::uint64_t injected_drop_count) noexcept {
        input_drop_count_at_burst_start = injected_drop_count;
        input_drop_burst_completed_at.reset();
    }

    void observe_input_drop_burst(const dots::client_runtime::PredictionStatistics& prediction,
                                  std::chrono::steady_clock::time_point now) noexcept {
        if (!input_drop_count_at_burst_start) {
            return;
        }
        if (prediction.injected_input_drop_count < *input_drop_count_at_burst_start) {
            input_drop_count_at_burst_start.reset();
            input_drop_burst_completed_at.reset();
            return;
        }
        const auto dropped_since_start =
            prediction.injected_input_drop_count - *input_drop_count_at_burst_start;
        if (!input_drop_burst_completed_at && prediction.pending_injected_input_drop_count == 0 &&
            dropped_since_start >= kInjectedInputDropBurstSize) {
            input_drop_burst_completed_at = now;
        }
        if (input_drop_burst_completed_at &&
            now - *input_drop_burst_completed_at >= kFaultCompletionReceiptDuration) {
            input_drop_count_at_burst_start.reset();
            input_drop_burst_completed_at.reset();
        }
    }
};

[[nodiscard]] constexpr std::string_view
connection_state_name(mycore::net_transport::ConnectionState state) noexcept {
    using mycore::net_transport::ConnectionState;
    switch (state) {
    case ConnectionState::Connecting:
        return "CONNECTING";
    case ConnectionState::Connected:
        return "CONNECTED";
    case ConnectionState::Closing:
        return "CLOSING";
    case ConnectionState::Disconnected:
        return "DISCONNECTED";
    case ConnectionState::Failed:
        return "FAILED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view
runtime_state_name(dots::client_runtime::State state) noexcept {
    using dots::client_runtime::State;
    switch (state) {
    case State::Connecting:
        return "CONNECTING";
    case State::Handshaking:
        return "HANDSHAKING";
    case State::Ready:
        return "READY";
    case State::Disconnected:
        return "DISCONNECTED";
    case State::Failed:
        return "FAILED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view
session_mode_name(dots::protocol::SessionMode mode) noexcept {
    using dots::protocol::SessionMode;
    switch (mode) {
    case SessionMode::Playing:
        return "PLAYING";
    case SessionMode::Spectating:
        return "SPECTATING";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view
respawn_result_name(dots::protocol::RespawnResult result) noexcept {
    using dots::protocol::RespawnResult;
    switch (result) {
    case RespawnResult::None:
        return "NONE";
    case RespawnResult::Accepted:
        return "ACCEPTED";
    case RespawnResult::RejectedCooldown:
        return "REJECTED: COOLDOWN";
    case RespawnResult::RejectedNotSpectating:
        return "REJECTED: NOT SPECTATING";
    case RespawnResult::RejectedNoSafeSpawn:
        return "REJECTED: NO SAFE SPAWN";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view
consequence_policy_name(mycore::rollback::ConsequencePolicy policy) noexcept {
    using mycore::rollback::ConsequencePolicy;
    switch (policy) {
    case ConsequencePolicy::PredictOnce:
        return "PREDICT ONCE";
    case ConsequencePolicy::PredictCancelable:
        return "CANCELABLE";
    case ConsequencePolicy::ConfirmOnce:
        return "CONFIRM ONCE";
    }
    return "UNKNOWN";
}

void draw_input_sequence(std::string_view label, dots::protocol::InputSequenceId value) {
    if (value.is_valid()) {
        ImGui::Text("%.*s: %u", static_cast<int>(label.size()), label.data(), value.value());
    } else {
        ImGui::Text("%.*s: none", static_cast<int>(label.size()), label.data());
    }
}

void draw_authority_receipt_sequence(std::string_view label,
                                     dots::protocol::AuthorityReceiptSequenceId value) {
    if (value.is_valid()) {
        ImGui::Text("%.*s: %u", static_cast<int>(label.size()), label.data(), value.value());
    } else {
        ImGui::Text("%.*s: none", static_cast<int>(label.size()), label.data());
    }
}

void draw_entity_id(std::string_view label, dots::protocol::EntityId value) {
    if (value.is_valid()) {
        ImGui::Text("%.*s: %u", static_cast<int>(label.size()), label.data(), value.value());
    } else {
        ImGui::Text("%.*s: none", static_cast<int>(label.size()), label.data());
    }
}

void draw_gameplay_debug_tab(const DebugWorldStats& world) {
    if (!world.gameplay_session) {
        mycore::debug_ui::description("Authoritative gameplay state requires a network session.");
        return;
    }

    const auto& gameplay = *world.gameplay_session;
    const auto mode = session_mode_name(gameplay.mode);
    ImGui::Text("Client ID: %u", gameplay.client_id.value());
    ImGui::Text("Session mode: %.*s", static_cast<int>(mode.size()), mode.data());
    ImGui::Text("Owned pieces: %zu", gameplay.owned_piece_count);
    draw_entity_id("Primary entity", gameplay.primary_entity_id);
    draw_entity_id("Killer / follow", gameplay.follow_entity_id);

    ImGui::Separator();
    if (gameplay.defeat_tick) {
        ImGui::Text("Defeat tick: %u", *gameplay.defeat_tick);
    } else {
        ImGui::TextUnformatted("Defeat tick: none");
    }
    if (gameplay.respawn_available_tick) {
        ImGui::Text("Respawn available tick: %u", *gameplay.respawn_available_tick);
        if (gameplay.respawn_seconds_remaining && *gameplay.respawn_seconds_remaining > 0.0) {
            ImGui::Text("Estimated countdown: %.1f s", *gameplay.respawn_seconds_remaining);
        } else if (gameplay.respawn_seconds_remaining) {
            ImGui::TextColored({0.35F, 0.9F, 0.45F, 1.0F}, "Estimated countdown: eligible");
        } else {
            ImGui::TextUnformatted("Estimated countdown: unavailable");
        }
    } else {
        ImGui::TextUnformatted("Respawn available tick: none");
        ImGui::TextUnformatted("Estimated countdown: unavailable");
    }
    mycore::debug_ui::description(
        "Countdown is presentation only; the server tick decides eligibility.");

    ImGui::Separator();
    ImGui::TextUnformatted("Latest authoritative absorption");
    if (gameplay.latest_absorption) {
        const auto& absorption = *gameplay.latest_absorption;
        ImGui::Text("Tick: %u", absorption.server_tick);
        ImGui::Text("Absorber / victim: %u / %u",
                    absorption.absorber_entity_id.value(),
                    absorption.victim_entity_id.value());
        ImGui::Text("Owners: %u / %u",
                    absorption.absorber_owner_id.value(),
                    absorption.victim_owner_id.value());
        ImGui::Text("Transferred mass: %.3f", absorption.transferred_mass);
    } else {
        ImGui::TextDisabled("None");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Latest authoritative respawn result");
    draw_input_sequence("Request input", gameplay.latest_respawn_request_id);
    const auto respawn_result = respawn_result_name(gameplay.latest_respawn_result);
    ImGui::Text("Result: %.*s", static_cast<int>(respawn_result.size()), respawn_result.data());
}

void draw_snapshot_sequence(std::string_view label, dots::protocol::SnapshotId value) {
    if (value.is_valid()) {
        ImGui::Text("%.*s: %u", static_cast<int>(label.size()), label.data(), value.value());
    } else {
        ImGui::Text("%.*s: none", static_cast<int>(label.size()), label.data());
    }
}

[[nodiscard]] ImVec4 history_utilization_color(float percent) noexcept {
    if (percent >= 90.0F) {
        return {1.0F, 0.2F, 0.25F, 1.0F};
    }
    if (percent >= 75.0F) {
        return {1.0F, 0.5F, 0.15F, 1.0F};
    }
    if (percent >= 50.0F) {
        return {0.95F, 0.8F, 0.2F, 1.0F};
    }
    return {0.35F, 0.9F, 0.45F, 1.0F};
}

void draw_runtime_debug_tab(const ClientConfig& config,
                            const DebugWorldStats& world,
                            const mycore::debug::FrameMetricsSnapshot& frame_metrics,
                            const mycore::debug::FixedStepMetricsSnapshot& simulation_metrics) {
    const auto input_mode = input_mode_name(config.controls.mode);
    ImGui::Text("Input: %.*s", static_cast<int>(input_mode.size()), input_mode.data());
    ImGui::Text("Presentation: %.*s",
                static_cast<int>(world.presentation.size()),
                world.presentation.data());
    ImGui::Text("Tick: %llu", static_cast<unsigned long long>(world.tick));
    ImGui::Text("Players: %zu", world.player_count);
    ImGui::Text("Food: %zu", world.food_count);
    if (world.occupied_grid_cells) {
        ImGui::Text("Grid cells: %zu", *world.occupied_grid_cells);
    }
    if (world.snapshot_id) {
        ImGui::Text("Snapshot: %u", *world.snapshot_id);
    }

    ImGui::Separator();
    ImGui::Text("Frame: %.2f ms", frame_metrics.latest_milliseconds);
    ImGui::Text("Average: %.2f ms", frame_metrics.average_milliseconds);
    ImGui::Text("FPS: %.1f", frame_metrics.frames_per_second);

    ImGui::Separator();
    const auto unhealthy = simulation_metrics.latest_step_limit_reached ||
                           simulation_metrics.latest_deadline_missed ||
                           simulation_metrics.latest_discarded_milliseconds > 0.0;
    if (unhealthy) {
        ImGui::TextColored({1.0F, 0.55F, 0.2F, 1.0F}, "Simulation health: OVERLOAD");
    } else {
        ImGui::TextColored({0.35F, 0.9F, 0.45F, 1.0F}, "Simulation health: OK");
    }
    ImGui::Text("Tick rate: %.1f / %.1f Hz",
                simulation_metrics.actual_steps_per_second,
                simulation_metrics.target_steps_per_second);
    ImGui::Text("Steps: %zu (excess: %zu)",
                simulation_metrics.latest_steps,
                simulation_metrics.latest_pending_steps);
    ImGui::Text("Simulation: %.2f ms (%.2f ms/step)",
                simulation_metrics.latest_simulation_milliseconds,
                simulation_metrics.latest_step_milliseconds);
    ImGui::Text("Backlog: %.2f ms", simulation_metrics.backlog_milliseconds);
    ImGui::Text("Catch-up / cap hits: %llu / %llu",
                static_cast<unsigned long long>(simulation_metrics.catch_up_frame_count),
                static_cast<unsigned long long>(simulation_metrics.step_limit_hit_count));
    ImGui::Text("Deadline misses: %llu",
                static_cast<unsigned long long>(simulation_metrics.deadline_miss_count));
    ImGui::Text("Discarded time: %.2f ms", simulation_metrics.total_discarded_milliseconds);
}

void draw_network_debug_tab(const DebugWorldStats& world) {
    if (world.network_session) {
        const auto& session = *world.network_session;
        ImGui::TextUnformatted("Session");
        const auto runtime_state = runtime_state_name(session.runtime_state);
        ImGui::Text("Runtime: %.*s", static_cast<int>(runtime_state.size()), runtime_state.data());
        if (world.transport) {
            const auto connection_state = connection_state_name(world.transport->state);
            ImGui::Text("Connection: %.*s",
                        static_cast<int>(connection_state.size()),
                        connection_state.data());
        } else {
            ImGui::TextUnformatted("Connection: unavailable");
        }
        ImGui::Text("Protocol: %u", static_cast<unsigned int>(dots::protocol::kProtocolVersion));
        ImGui::Text("Client ID: %u", session.client_id.value());
        if (session.controlled_entity_id.is_valid()) {
            ImGui::Text("Controlled entity: %u", session.controlled_entity_id.value());
        } else {
            ImGui::TextUnformatted("Controlled entity: none");
        }
        ImGui::Text("Connection handle: %u", session.connection_handle.value());
        ImGui::Text(
            "Snapshot / server tick: %u / %u", world.snapshot_id.value_or(0), session.server_tick);
        ImGui::Text("Local input tick (next): %u", session.local_input_tick);
        mycore::debug_ui::description("Local and server ticks are not synchronized.");
    } else {
        mycore::debug_ui::description("No network session in offline presentation mode.");
    }

    if (world.replication) {
        ImGui::Separator();
        ImGui::TextUnformatted("Replication");
        if (world.replication->latest_snapshot_age) {
            ImGui::Text("Snapshot age: %lld ms",
                        static_cast<long long>(world.replication->latest_snapshot_age->count()));
        } else {
            ImGui::TextUnformatted("Snapshot age: unavailable");
        }
        ImGui::Text("Receive rate: %.1f snapshots/s",
                    world.replication->accepted_snapshots_per_second);
    }

    if (world.transport) {
        ImGui::Separator();
        ImGui::TextUnformatted("Transport");
        const auto state = connection_state_name(world.transport->state);
        ImGui::Text("State: %.*s", static_cast<int>(state.size()), state.data());
        if (world.transport->round_trip_time) {
            ImGui::Text("RTT: %lld ms",
                        static_cast<long long>(world.transport->round_trip_time->count()));
        } else {
            ImGui::TextUnformatted("RTT: unavailable");
        }
        if (world.transport->packet_loss_percent) {
            ImGui::Text("Packet loss: %.2f%%", *world.transport->packet_loss_percent);
        } else {
            ImGui::TextUnformatted("Packet loss: unavailable");
        }
        if (world.transport->inbound_bytes_per_second &&
            world.transport->outbound_bytes_per_second &&
            world.transport->inbound_packets_per_second &&
            world.transport->outbound_packets_per_second) {
            ImGui::Text("Bytes/s in / out: %.0f / %.0f",
                        *world.transport->inbound_bytes_per_second,
                        *world.transport->outbound_bytes_per_second);
            ImGui::Text("Packets/s in / out: %.1f / %.1f",
                        *world.transport->inbound_packets_per_second,
                        *world.transport->outbound_packets_per_second);
        } else {
            ImGui::TextUnformatted("Rates: unavailable");
        }
        if (world.transport->pending_reliable_bytes && world.transport->pending_unreliable_bytes) {
            ImGui::Text("Queued reliable / unreliable: %zu / %zu bytes",
                        *world.transport->pending_reliable_bytes,
                        *world.transport->pending_unreliable_bytes);
        } else {
            ImGui::TextUnformatted("Queues: unavailable");
        }
        if (world.transport->sent_unacknowledged_reliable_bytes) {
            ImGui::Text("Reliable sent unacked: %zu bytes",
                        *world.transport->sent_unacknowledged_reliable_bytes);
        }
        if (world.transport->outbound_queue_delay) {
            const auto delay =
                std::chrono::duration<double, std::milli>{*world.transport->outbound_queue_delay};
            ImGui::Text("Queue delay: %.2f ms", delay.count());
        }
    }
}

void draw_prediction_debug_tab(const DebugWorldStats& world) {
    if (!world.network_session) {
        mycore::debug_ui::description("Prediction diagnostics require a network session.");
        return;
    }

    const auto& session = *world.network_session;
    const auto& prediction = session.prediction;
    ImGui::TextUnformatted("Authority receipt publication");
    draw_authority_receipt_sequence("Accepted through",
                                    prediction.authority_receipts_accepted_through);
    draw_authority_receipt_sequence("Published through",
                                    prediction.authority_receipts_published_through);
    draw_authority_receipt_sequence("Server retired through",
                                    prediction.authority_receipts_server_retired_through);
    ImGui::Text("Retained / awaiting publication: %zu / %zu",
                prediction.authority_receipt_retained_count,
                prediction.authority_receipt_pending_publication_count);
    ImGui::Text("Queued event batches: %zu", prediction.pending_prediction_event_batch_count);
    const auto& consequences = session.consequences;
    ImGui::Separator();
    ImGui::TextUnformatted("Rollback consequences");
    ImGui::Text("Batches / visible cues / stinger: %llu / %zu / %llu",
                static_cast<unsigned long long>(consequences.consumed_batch_count),
                consequences.visible_cue_count,
                static_cast<unsigned long long>(consequences.stinger_sequence));
    ImGui::Text("Transitions first / revised / retracted / confirmed / authority: "
                "%llu / %llu / %llu / %llu / %llu",
                static_cast<unsigned long long>(consequences.transition_counts[0]),
                static_cast<unsigned long long>(consequences.transition_counts[1]),
                static_cast<unsigned long long>(consequences.transition_counts[2]),
                static_cast<unsigned long long>(consequences.transition_counts[3]),
                static_cast<unsigned long long>(consequences.transition_counts[4]));
    for (const auto& handler : consequences.handlers) {
        const auto policy = consequence_policy_name(handler.policy);
        const auto& values = handler.statistics;
        ImGui::Text("H%zu %.*s d/s/r/x/c/f: %llu/%llu/%llu/%llu/%llu/%llu",
                    handler.handler_index,
                    static_cast<int>(policy.size()),
                    policy.data(),
                    static_cast<unsigned long long>(values.delivered_count),
                    static_cast<unsigned long long>(values.suppressed_count),
                    static_cast<unsigned long long>(values.revised_count),
                    static_cast<unsigned long long>(values.canceled_count),
                    static_cast<unsigned long long>(values.confirmed_count),
                    static_cast<unsigned long long>(values.failure_count));
    }

    if (!session.local_prediction_available || !session.latest_authoritative_sample ||
        !session.predicted_position || !session.presentation_position ||
        !session.smoothing_offset) {
        mycore::debug_ui::description(
            "Local prediction diagnostics are unavailable while spectating.");
        return;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Input history and rollback");
    ImGui::Text("Redundancy: %s", prediction.input_redundancy_enabled ? "ENABLED" : "DISABLED");
    draw_input_sequence("Last input sent", prediction.last_input_sent);
    draw_input_sequence("Last input acknowledged", prediction.last_input_acknowledged);
    ImGui::Text("Command lead: %zu", prediction.unacknowledged_input_count);
    const auto history_percent = prediction.history_capacity > 0
                                     ? (100.0F * static_cast<float>(prediction.history_count)) /
                                           static_cast<float>(prediction.history_capacity)
                                     : 0.0F;
    ImGui::TextColored(history_utilization_color(history_percent),
                       "History: %zu / %zu (%.1f%%), high %zu",
                       prediction.history_count,
                       prediction.history_capacity,
                       history_percent,
                       prediction.history_high_water_mark);
    ImGui::Text("Scope epoch / horizon: %llu / %llu ticks",
                static_cast<unsigned long long>(prediction.scope_epoch),
                static_cast<unsigned long long>(prediction.scope_replay_horizon_ticks));
    ImGui::Text("Scope owners / event owners / players / food: %zu / %zu / %zu / %zu",
                prediction.scope_owner_count,
                prediction.scope_event_owner_count,
                prediction.scope_player_count,
                prediction.scope_food_count);
    ImGui::Text("Scope rebases: %llu",
                static_cast<unsigned long long>(prediction.scope_rebase_count));
    ImGui::Text("Server pending: %u, high %u",
                static_cast<unsigned int>(prediction.latest_server_pending_input_count),
                static_cast<unsigned int>(prediction.server_pending_input_high_water_mark));
    draw_snapshot_sequence("Rollback snapshot", prediction.rollback_snapshot_id);
    ImGui::Text("Rollback server tick: %u", prediction.rollback_server_tick);
    draw_input_sequence("Rollback input ACK", prediction.rollback_input_acknowledgement);
    ImGui::Text("Replay last / total / max: %zu / %llu / %zu",
                prediction.latest_replay_count,
                static_cast<unsigned long long>(prediction.total_replayed_input_count),
                prediction.maximum_replay_count);
    ImGui::Text("Replay ms last / avg / max: %.3f / %.3f / %.3f",
                prediction.latest_replay_milliseconds,
                prediction.average_replay_milliseconds,
                prediction.maximum_replay_milliseconds);
    ImGui::Text("Replay over budget / hard resync: %llu / %llu",
                static_cast<unsigned long long>(prediction.replay_over_budget_count),
                static_cast<unsigned long long>(prediction.hard_resync_count));

    ImGui::Separator();
    ImGui::TextUnformatted("Correction and presentation");
    ImGui::Text("Reconciliations / corrections: %llu / %llu",
                static_cast<unsigned long long>(prediction.reconciliation_count),
                static_cast<unsigned long long>(prediction.nonzero_correction_count));
    ImGui::Text("Remote entity corrections last / total: %zu / %llu",
                prediction.latest_remote_entity_correction_count,
                static_cast<unsigned long long>(prediction.remote_entity_correction_count));
    ImGui::Text("Correction last / max: %.4f / %.4f units",
                prediction.latest_correction_distance,
                prediction.maximum_correction_distance);
    ImGui::Text("Remote correction last / max: %.4f / %.4f units",
                prediction.latest_remote_correction_distance,
                prediction.maximum_remote_correction_distance);
    ImGui::Text("Corrections/min: %.0f", prediction.corrections_per_minute);
    ImGui::Text("Correction ghosts: %zu / %zu (local %zu, remote %zu)",
                session.retained_correction_count,
                session.correction_history_capacity,
                session.retained_local_correction_count,
                session.retained_remote_correction_count);
    const auto& persistent = session.persistent_presentation;
    ImGui::Text("Presentation tracks / fades / trail: %zu / %zu / %zu",
                persistent.track_count,
                persistent.structural_fade_count,
                persistent.motion_trail_count);
    ImGui::Text("Source handoffs / smoothed / remaps: %llu / %llu / %llu",
                static_cast<unsigned long long>(persistent.source_handoff_count),
                static_cast<unsigned long long>(persistent.smoothed_correction_count),
                static_cast<unsigned long long>(persistent.identity_remap_count));
    ImGui::Text("Authority sample: (%.3f, %.3f)",
                session.latest_authoritative_sample->x,
                session.latest_authoritative_sample->y);
    ImGui::Text(
        "Predicted: (%.3f, %.3f)", session.predicted_position->x, session.predicted_position->y);
    ImGui::Text("Presentation: (%.3f, %.3f)",
                session.presentation_position->x,
                session.presentation_position->y);
    ImGui::Text("Smoothing offset: (%.3f, %.3f), |v| %.4f",
                session.smoothing_offset->x,
                session.smoothing_offset->y,
                mycore::math::length(*session.smoothing_offset));
    ImGui::Text("Injected drops / errors: %llu / %llu",
                static_cast<unsigned long long>(prediction.injected_input_drop_count),
                static_cast<unsigned long long>(prediction.injected_prediction_error_count));
}

void draw_interpolation_debug_tab(const DebugWorldStats& world) {
    ImGui::TextUnformatted("Remote presentation");
    if (!world.network_session) {
        mycore::debug_ui::description("Interpolation diagnostics require a network session.");
        return;
    }
    const auto& session = *world.network_session;
    const auto& remote = session.remote_presentation;
    const auto remote_mode = remote_presentation_mode_name(session.remote_presentation_mode);
    ImGui::Text("Playing mode: %.*s", static_cast<int>(remote_mode.size()), remote_mode.data());
    const auto& extrapolation = session.remote_extrapolation;
    ImGui::Text("Extrapolation age / ticks: %.1f ms / %.2f",
                extrapolation.sample_age_milliseconds,
                extrapolation.extrapolation_ticks);
    ImGui::Text("Extrapolating / held / static: %zu / %zu / %zu",
                extrapolation.extrapolated_player_count,
                extrapolation.held_player_count,
                extrapolation.static_entity_count);
    ImGui::Text("Extrapolation samples accepted / rejected: %llu / %llu",
                static_cast<unsigned long long>(extrapolation.accepted_snapshot_count),
                static_cast<unsigned long long>(extrapolation.rejected_snapshot_count));
    ImGui::Separator();
    ImGui::TextUnformatted("Delayed interpolation fallback");
    ImGui::Text("Buffer: %zu / %zu", remote.sample_count, remote.sample_capacity);
    ImGui::Text(
        "Coverage: %u ticks / %.1f ms", remote.coverage_ticks, remote.coverage_milliseconds);
    ImGui::Text("Delay target / current: %.2f / %.2f ticks",
                remote.target_delay_ticks,
                remote.current_delay_ticks);
    ImGui::Text("Presentation tick: %.3f", remote.presentation_tick);
    ImGui::Text("Cursor rate / error: %.3f / %.3f ticks", remote.cursor_rate, remote.cursor_error);
    if (remote.bracket) {
        ImGui::Text("Brackets: %u@%u -> %u@%u, alpha %.3f",
                    remote.bracket->older_snapshot_id.value(),
                    remote.bracket->older_server_tick,
                    remote.bracket->newer_snapshot_id.value(),
                    remote.bracket->newer_server_tick,
                    remote.bracket->alpha);
    } else {
        ImGui::TextUnformatted("Brackets: unavailable (startup or hold)");
    }
    ImGui::Text("Jitter latest / EWMA: %.2f / %.2f ms",
                remote.latest_jitter_milliseconds,
                remote.ewma_jitter_milliseconds);
    ImGui::Text("Late snapshots: %llu",
                static_cast<unsigned long long>(remote.late_snapshot_count));
    ImGui::Text("Holding: %s", remote.holding ? "YES" : "NO");
    ImGui::Text("Hold episodes / recoveries: %llu / %llu",
                static_cast<unsigned long long>(remote.hold_episode_count),
                static_cast<unsigned long long>(remote.hold_recovery_count));
    ImGui::Text("Hold current / last: %lld / %lld ms",
                static_cast<long long>(remote.current_hold_duration.count()),
                static_cast<long long>(remote.last_hold_duration.count()));
    ImGui::Text("Hold maximum / total: %lld / %lld ms",
                static_cast<long long>(remote.maximum_hold_duration.count()),
                static_cast<long long>(remote.total_hold_duration.count()));
    ImGui::Text("Rate corrections / rebases: %llu / %llu",
                static_cast<unsigned long long>(remote.rate_correction_count),
                static_cast<unsigned long long>(remote.hard_rebase_count));
    ImGui::Text("Delayed creates / removes: %llu / %llu",
                static_cast<unsigned long long>(remote.delayed_entity_create_count),
                static_cast<unsigned long long>(remote.delayed_entity_remove_count));
    if (session.representative_remote_entity) {
        ImGui::Text("Example remote entity: %u", session.representative_remote_entity->value());
        if (session.representative_remote_endpoints.older) {
            const auto& older = *session.representative_remote_endpoints.older;
            ImGui::Text("Older endpoint: (%.3f, %.3f), mass %.3f",
                        older.position.x,
                        older.position.y,
                        older.mass);
        }
        if (session.representative_remote_endpoints.newer) {
            const auto& newer = *session.representative_remote_endpoints.newer;
            ImGui::Text("Newer endpoint: (%.3f, %.3f), mass %.3f",
                        newer.position.x,
                        newer.position.y,
                        newer.mass);
        }
    } else {
        ImGui::TextUnformatted("Example remote entity: unavailable");
    }
    mycore::debug_ui::description(
        "Playing extrapolation advances only known movement and launch for six ticks, then "
        "holds. Spectators and the comparison layer use delayed known authority.");
}

void draw_prediction_tools_tab(const DebugWorldStats& world,
                               PredictionDebugControls* prediction_controls) {
    if (!world.network_session) {
        mycore::debug_ui::description("Prediction tools require a network session.");
        return;
    }

    if (prediction_controls == nullptr) {
        mycore::debug_ui::description("Prediction tools are unavailable.");
        return;
    }

    const auto& session = *world.network_session;
    const auto& prediction = session.prediction;
    if (session.local_prediction_available) {
        ImGui::TextUnformatted("Fault injection");
        if (prediction_controls->input_drop_count_at_burst_start) {
            const auto dropped_since_start =
                prediction.injected_input_drop_count -
                std::min(prediction.injected_input_drop_count,
                         *prediction_controls->input_drop_count_at_burst_start);
            if (prediction.pending_injected_input_drop_count > 0) {
                ImGui::TextColored({1.0F, 0.65F, 0.2F, 1.0F},
                                   "Injected drop burst: %llu / %zu dropped (%zu remaining)",
                                   static_cast<unsigned long long>(dropped_since_start),
                                   kInjectedInputDropBurstSize,
                                   prediction.pending_injected_input_drop_count);
            } else if (prediction_controls->input_drop_burst_completed_at) {
                ImGui::TextColored({0.35F, 0.9F, 0.45F, 1.0F},
                                   "Last fault: dropped %zu input packets (complete)",
                                   kInjectedInputDropBurstSize);
            }
        }
        if (ImGui::Button("Inject +1 X error")) {
            prediction_controls->requested_prediction_error = Vector2{1.0F, 0.0F};
        }
        ImGui::SameLine();
        if (ImGui::Button("Inject +1 Y error")) {
            prediction_controls->requested_prediction_error = Vector2{0.0F, 1.0F};
        }
        auto relative_error = std::optional<Vector2>{};
        if (session.last_nonzero_movement_input) {
            relative_error = mycore::math::normalized_or_zero(*session.last_nonzero_movement_input);
        }
        ImGui::BeginDisabled(!relative_error.has_value());
        if (ImGui::Button("Force +1 position drift along last movement") && relative_error) {
            prediction_controls->requested_prediction_error = *relative_error;
        }
        ImGui::EndDisabled();
        if (session.last_nonzero_movement_input) {
            ImGui::Text("Last nonzero input: (%.3f, %.3f)",
                        session.last_nonzero_movement_input->x,
                        session.last_nonzero_movement_input->y);
        } else {
            ImGui::TextDisabled("Last nonzero input: none yet");
        }
        ImGui::BeginDisabled(prediction.pending_injected_input_drop_count > 0);
        if (ImGui::Button("Drop next 3 input packets")) {
            prediction_controls->drop_input_packets_requested = true;
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted("Visual layers");
        ImGui::Checkbox("Show prediction layers", &prediction_controls->show_prediction_layers);
        ImGui::Checkbox("Show correction replay", &prediction_controls->show_replay_path);
        ImGui::Checkbox("Show remote endpoint outlines",
                        &prediction_controls->show_remote_endpoint_layers);
        if (ImGui::Button("Clear correction ghosts")) {
            prediction_controls->clear_correction_visuals_requested = true;
        }
        mycore::debug_ui::description("White: predicted position");
        mycore::debug_ui::description("Orange: latest authoritative sample");
        mycore::debug_ui::description(
            "Magenta: local/remote pre-correction (fades with age); Purple: replay");
        mycore::debug_ui::description("Cyan / Blue: older / newer remote snapshot");
        mycore::debug_ui::description("Fill: presentation position");
        return;
    }

    ImGui::TextUnformatted("Remote presentation");
    ImGui::Checkbox("Show remote endpoint outlines",
                    &prediction_controls->show_remote_endpoint_layers);
    mycore::debug_ui::description("Cyan / Blue: older / newer remote snapshot");
    mycore::debug_ui::description("Fill: delayed presentation position");
}

void draw_debug_overlay(const ClientConfig& config,
                        const DebugWorldStats& world,
                        const mycore::debug::FrameMetricsSnapshot& frame_metrics,
                        const mycore::debug::FixedStepMetricsSnapshot& simulation_metrics,
                        PredictionDebugControls* prediction_controls = nullptr) {
    constexpr float kMargin = 12.0F;
    constexpr float kPreferredOverlayWidth = 360.0F;
    const auto* viewport = ImGui::GetMainViewport();
    const auto available_width = std::max(viewport->WorkSize.x - (2.0F * kMargin), 1.0F);
    const auto available_height = std::max(viewport->WorkSize.y - (2.0F * kMargin), 1.0F);
    const auto overlay_width = std::min(kPreferredOverlayWidth, available_width);
    constexpr auto kWindowFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::SetNextWindowPos({viewport->WorkPos.x + kMargin, viewport->WorkPos.y + kMargin},
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82F);
    if (ImGui::Begin("Dots game state", nullptr, kWindowFlags)) {
        ImGui::Text("Players: %zu", world.player_count);
        if (world.current_player_position) {
            ImGui::Text("Authoritative player: (%.2f, %.2f)",
                        world.current_player_position->x,
                        world.current_player_position->y);
        } else {
            ImGui::TextDisabled("Authoritative player: unavailable");
        }
        if (world.gameplay_session && world.gameplay_session->respawn_available_tick) {
            const auto& gameplay = *world.gameplay_session;
            if (gameplay.respawn_seconds_remaining && *gameplay.respawn_seconds_remaining > 0.0) {
                ImGui::TextColored({1.0F, 0.75F, 0.25F, 1.0F},
                                   "Respawn: %.1f s (tick %u)",
                                   *gameplay.respawn_seconds_remaining,
                                   *gameplay.respawn_available_tick);
            } else if (gameplay.respawn_seconds_remaining) {
                ImGui::TextColored({0.35F, 0.9F, 0.45F, 1.0F},
                                   "Respawn: eligible (tick %u)",
                                   *gameplay.respawn_available_tick);
            } else {
                ImGui::Text("Respawn: unavailable (tick %u)", *gameplay.respawn_available_tick);
            }
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos(
        {viewport->WorkPos.x + kMargin, viewport->WorkPos.y + viewport->WorkSize.y - kMargin},
        ImGuiCond_Always,
        {0.0F, 1.0F});
    ImGui::SetNextWindowSizeConstraints({overlay_width, 0.0F}, {overlay_width, available_height});
    ImGui::SetNextWindowBgAlpha(0.82F);
    if (ImGui::Begin("Dots session", nullptr, kWindowFlags)) {
        if (ImGui::BeginTabBar("Dots session tabs")) {
            if (ImGui::BeginTabItem("Runtime")) {
                draw_runtime_debug_tab(config, world, frame_metrics, simulation_metrics);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Network")) {
                draw_network_debug_tab(world);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gameplay")) {
                draw_gameplay_debug_tab(world);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - kMargin,
                             viewport->WorkPos.y + viewport->WorkSize.y - kMargin},
                            ImGuiCond_Always,
                            {1.0F, 1.0F});
    ImGui::SetNextWindowSizeConstraints({overlay_width, 0.0F}, {overlay_width, available_height});
    ImGui::SetNextWindowBgAlpha(0.82F);
    if (ImGui::Begin("Dots diagnostics", nullptr, kWindowFlags)) {
        if (ImGui::BeginTabBar("Dots diagnostics tabs")) {
            if (ImGui::BeginTabItem("Prediction")) {
                draw_prediction_debug_tab(world);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Interpolation")) {
                draw_interpolation_debug_tab(world);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tools")) {
                draw_prediction_tools_tab(world, prediction_controls);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void draw_confirmed_notice(const std::optional<dots::presentation::ConfirmedNotice>& notice) {
    if (!notice) {
        return;
    }
    const auto* viewport = ImGui::GetMainViewport();
    constexpr auto kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos(
        {viewport->WorkPos.x + (viewport->WorkSize.x * 0.5F), viewport->WorkPos.y + 42.0F},
        ImGuiCond_Always,
        {0.5F, 0.0F});
    ImGui::SetNextWindowBgAlpha(0.72F * notice->opacity);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0F, 0.82F, 0.35F, notice->opacity});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.0F, 12.0F});
    if (ImGui::Begin("Dots confirmed consequence", nullptr, kFlags)) {
        const auto text = dots::presentation::confirmed_notice_text(notice->kind);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

class SimulationHealthReporter {
public:
    void update(const mycore::debug::FixedStepMetricsSnapshot& metrics,
                std::chrono::steady_clock::time_point now) {
        constexpr auto kWarningInterval = std::chrono::seconds{5};
        constexpr auto kErrorThreshold = std::chrono::seconds{10};
        constexpr std::size_t kRecoveryFrameCount = 30;
        const auto unhealthy = metrics.latest_step_limit_reached ||
                               metrics.latest_deadline_missed ||
                               metrics.latest_discarded_milliseconds > 0.0;
        if (unhealthy) {
            healthy_frame_count_ = 0;
            if (!unhealthy_) {
                unhealthy_since_ = now;
                error_reported_ = false;
            }
            if (!unhealthy_ || now - last_warning_ >= kWarningInterval) {
                mycore::debug::log_warning(
                    "dots.client.simulation",
                    "Fixed-step overload: rate {:.1f}/{:.1f} Hz, steps {}, excess {}, "
                    "step {:.2f} ms, backlog {:.2f} ms, discarded {:.2f} ms",
                    metrics.actual_steps_per_second,
                    metrics.target_steps_per_second,
                    metrics.latest_steps,
                    metrics.latest_pending_steps,
                    metrics.latest_step_milliseconds,
                    metrics.backlog_milliseconds,
                    metrics.latest_discarded_milliseconds);
                last_warning_ = now;
            }
            if (!error_reported_ && now - unhealthy_since_ >= kErrorThreshold) {
                mycore::debug::log_error(
                    "dots.client.simulation",
                    "Fixed-step overload has persisted for 10 seconds; current rate is "
                    "{:.1f}/{:.1f} Hz with {} deadline misses and {} cap hits",
                    metrics.actual_steps_per_second,
                    metrics.target_steps_per_second,
                    metrics.deadline_miss_count,
                    metrics.step_limit_hit_count);
                error_reported_ = true;
            }
            unhealthy_ = true;
            return;
        }

        if (unhealthy_ && ++healthy_frame_count_ >= kRecoveryFrameCount) {
            mycore::debug::log_info("dots.client.simulation",
                                    "Fixed-step timing recovered at {:.1f}/{:.1f} Hz",
                                    metrics.actual_steps_per_second,
                                    metrics.target_steps_per_second);
            unhealthy_ = false;
            error_reported_ = false;
            healthy_frame_count_ = 0;
        }
    }

private:
    std::chrono::steady_clock::time_point last_warning_{};
    std::chrono::steady_clock::time_point unhealthy_since_{};
    std::size_t healthy_frame_count_{};
    bool unhealthy_{};
    bool error_reported_{};
};

void validate_render_assets(const mycore::assets::DirectorySource& assets) {
    constexpr std::array shader_names{
        "mycore/render_2d/shaders/circle.vert",
        "mycore/render_2d/shaders/circle.frag",
        "mycore/render_2d/shaders/grid.vert",
        "mycore/render_2d/shaders/grid.frag",
    };
    const auto extension =
        mycore::render::shader_file_extension(mycore::render::platform_shader_format());
    for (const auto* shader_name : shader_names) {
        const auto asset_name = std::string{shader_name} + "." + std::string{extension};
        if (assets.read(asset_name).empty()) {
            throw dots::client::StartupError{"Packaged shader is empty: " + asset_name};
        }
    }
}

int run_networked_game(
    const ClientConfig& config,
    mycore::platform_sdl::Window& window,
    mycore::render_2d::Renderer& renderer,
    mycore::debug_ui::Context& debug_ui,
    const dots::presentation::Settings& render_settings,
    mycore::net_transport::Endpoint& endpoint,
    dots::server::Runtime* embedded_server,
    std::optional<std::chrono::milliseconds> graceful_disconnect_drain = std::nullopt) {
    using namespace std::chrono_literals;
    dots::client_runtime::Runtime client{
        endpoint,
        {
            .input_redundancy = config.network.input_redundancy,
            .log_prediction_scope_changes =
                config.debug.prediction_log_level != PredictionLogLevel::Off,
            .log_prediction_reconciliation_details =
                config.debug.prediction_log_level == PredictionLogLevel::Debug,
        },
    };
    dots::presentation::RemoteSnapshotBuffer remote_snapshot_buffer;
    dots::presentation::RemoteExtrapolationBuffer remote_extrapolation_buffer;
    dots::presentation::RollbackConsequencePresentation consequence_presentation;
    auto consequence_delivery_ready = false;
    const auto drain_consequence_batches = [&](std::chrono::steady_clock::time_point observed_at) {
        if (!consequence_delivery_ready) {
            return;
        }
        for (const auto& batch : client.take_prediction_event_batches()) {
            const auto report = consequence_presentation.consume(batch, observed_at);
            if (!report.failures.empty()) {
                mycore::debug::log_error(
                    "dots.client.consequence",
                    "Rollback consequence batch reported {} non-retried handler failures",
                    report.failures.size());
            }
        }
    };
    const auto process_client_events = [&](std::chrono::steady_clock::time_point now) {
        auto result = client.process_events(now);
        for (auto& accepted : result.accepted_snapshots) {
            auto interpolation_entities = accepted.snapshot.entities;
            remote_snapshot_buffer.insert({
                .snapshot_id = accepted.snapshot.snapshot_id,
                .server_tick = accepted.snapshot.server_tick,
                .entities = std::move(interpolation_entities),
                .arrival_time = accepted.arrival_time,
            });
            if (const auto rules = client.world_rules()) {
                const auto owner_count = accepted.snapshot.owners.size();
                const auto entity_count = accepted.snapshot.entities.size();
                const auto inserted = remote_extrapolation_buffer.insert({
                    .snapshot_id = accepted.snapshot.snapshot_id,
                    .server_tick = accepted.snapshot.server_tick,
                    .rules = dots::replication::to_simulation(*rules),
                    .owners = std::move(accepted.snapshot.owners),
                    .entities = std::move(accepted.snapshot.entities),
                    .arrival_time = accepted.arrival_time,
                });
                if (!inserted) {
                    const auto previous = remote_extrapolation_buffer.statistics(now);
                    mycore::debug::log_error("dots.client.presentation",
                                             "Rejected extrapolation sample {} at server tick {} "
                                             "(previous sample {}, owners {}, entities {})",
                                             accepted.snapshot.snapshot_id.value(),
                                             accepted.snapshot.server_tick,
                                             previous.snapshot_id.value(),
                                             owner_count,
                                             entity_count);
                    result.error = dots::client_runtime::RuntimeError::InvalidSnapshot;
                    break;
                }
            }
        }
        drain_consequence_batches(now);
        if (result.error) {
            const auto name = dots::client_runtime::runtime_error_name(*result.error);
            mycore::debug::log_error(
                "dots.client.session", "Client runtime processing failed: {}", name);
        }
        return result.error.has_value();
    };
    const auto handshake_deadline = std::chrono::steady_clock::now() + 10s;
    while (client.state() != dots::client_runtime::State::Ready) {
        if (process_client_events(std::chrono::steady_clock::now())) {
            throw StartupError{"The networked client handshake failed"};
        }
        if (embedded_server != nullptr && embedded_server->process_events()) {
            throw StartupError{"The embedded authoritative server handshake failed"};
        }
        if (process_client_events(std::chrono::steady_clock::now())) {
            throw StartupError{"The networked client handshake failed"};
        }
        if (client.state() == dots::client_runtime::State::Disconnected ||
            client.state() == dots::client_runtime::State::Failed ||
            std::chrono::steady_clock::now() >= handshake_deadline) {
            throw StartupError{"Could not establish the authoritative session"};
        }
        std::this_thread::sleep_for(1ms);
    }
    if (const auto* controlled = client.world().find(client.primary_entity_id())) {
        consequence_presentation.set_local_owner(
            dots::simulation::PlayerOwnerId{controlled->owner_id.value()});
    }
    consequence_delivery_ready = true;
    drain_consequence_batches(std::chrono::steady_clock::now());

    mycore::time::FixedStepAccumulator accumulator{dots::simulation::kTickDuration};
    auto previous_time = std::chrono::steady_clock::now();
    std::uint32_t client_tick{};
    const auto maximum_frame_delta = std::chrono::duration_cast<mycore::time::Duration>(
        std::chrono::milliseconds{config.simulation.max_frame_delta_ms});
    mycore::debug::FrameMetrics frame_metrics;
    mycore::debug::FixedStepMetrics simulation_metrics{dots::simulation::kTickDuration};
    SimulationHealthReporter simulation_health_reporter;
    dots::presentation::LocalPredictionPresentation local_prediction_presentation;
    dots::presentation::PersistentWorldPresentation persistent_world_presentation;
    dots::presentation::PredictionCorrectionHistory prediction_correction_history{
        config.debug.correction_history_count};
    std::vector<dots::presentation::PredictionCorrectionSample> prediction_correction_samples;
    dots::presentation::SpectatorCamera spectator_camera{{
        .pan_speed_world_units_per_second = config.spectator.pan_speed_world_units_per_second,
        .minimum_pixels_per_world_unit = config.spectator.minimum_pixels_per_world_unit,
        .maximum_pixels_per_world_unit = config.spectator.maximum_pixels_per_world_unit,
    }};
    PlayerControlTracker player_control_tracker;
    SpectatorControlTracker spectator_control_tracker;
    PredictionDebugControls prediction_debug_controls;
    std::optional<Vector2> last_nonzero_movement_input;
    Vector2 last_camera_position;
    bool spectator_camera_active{};
    bool split_request_pending{};
    bool respawn_request_pending{};
    while (true) {
        MYCORE_PROFILE_FRAME();
        MYCORE_PROFILE_ZONE("Dots networked client frame");
        const auto input =
            mycore::platform_sdl::poll_input(window, config.debug.enabled ? &debug_ui : nullptr);
        if (quit_requested(input, config.controls)) {
            if (!client.disconnect()) {
                mycore::debug::log_warning(
                    "dots.client.session",
                    "Could not request a graceful disconnect for connection {}",
                    client.connection_handle().value());
            } else if (graceful_disconnect_drain) {
                mycore::debug::log_info("dots.client.session",
                                        "Draining native disconnect for {} ms",
                                        graceful_disconnect_drain->count());
                const auto deadline = std::chrono::steady_clock::now() + *graceful_disconnect_drain;
                while (std::chrono::steady_clock::now() < deadline) {
                    static_cast<void>(endpoint.poll());
                    std::this_thread::sleep_for(1ms);
                }
            }
            return 0;
        }

        const auto output_size = window.pixel_size();
        const auto logical_size = window.size();
        const auto render_surface_available = output_size.width > 0 && output_size.height > 0 &&
                                              logical_size.width > 0 && logical_size.height > 0;

        const auto now = std::chrono::steady_clock::now();
        if (process_client_events(now)) {
            throw StartupError{"The networked authoritative session failed"};
        }
        if (client.state() != dots::client_runtime::State::Ready) {
            throw StartupError{"The networked authoritative session disconnected"};
        }
        if (render_surface_available) {
            debug_ui.begin_frame();
        }
        const auto mouse_input_available =
            render_surface_available && (!config.debug.enabled || !debug_ui.wants_mouse_capture());
        auto spectator_input = input;
        if (!mouse_input_available) {
            spectator_input.wheel_delta_y = 0.0F;
        }
        const auto spectator_control_intent =
            spectator_control_tracker.sample(spectator_input, config.controls);
        const auto player_control_intent = player_control_tracker.sample(input, config.controls);
        const auto frame_duration =
            std::chrono::duration_cast<mycore::time::Duration>(now - previous_time);
        frame_metrics.add_sample(frame_duration);
        const auto elapsed = std::min(frame_duration, maximum_frame_delta);
        const auto discarded_frame_time = frame_duration - elapsed;
        previous_time = now;
        const auto step_result =
            accumulator.advance(elapsed, config.simulation.max_steps_per_frame);
        const auto discarded_backlog = step_result.step_limit_reached
                                           ? accumulator.discard_pending_steps()
                                           : mycore::time::Duration::zero();

        const auto playing_before_steps =
            client.session_mode() == dots::protocol::SessionMode::Playing;
        const auto* controlled =
            playing_before_steps ? client.world().find(client.primary_entity_id()) : nullptr;
        if (playing_before_steps && controlled == nullptr) {
            throw StartupError{"The replicated local player disappeared"};
        }
        if (playing_before_steps) {
            respawn_request_pending = false;
            split_request_pending = split_request_pending || player_control_intent.request_split;
        } else if (spectator_control_intent.request_respawn) {
            split_request_pending = false;
            respawn_request_pending = true;
        }
        auto viewport = InputViewport{};
        if (render_surface_available) {
            viewport = {
                .width = static_cast<float>(output_size.width),
                .height = static_cast<float>(output_size.height),
                .mouse_scale_x =
                    static_cast<float>(output_size.width) / static_cast<float>(logical_size.width),
                .mouse_scale_y = static_cast<float>(output_size.height) /
                                 static_cast<float>(logical_size.height),
                .player_radius_pixels = controlled == nullptr
                                            ? 0.0F
                                            : dots::simulation::radius_for_mass(controlled->mass) *
                                                  config.view.pixels_per_world_unit,
            };
        }

        const auto simulation_start = std::chrono::steady_clock::now();
        {
            MYCORE_PROFILE_ZONE("Dots network input steps");
            for (std::size_t step = 0; step < step_result.steps; ++step) {
                if (client_tick == std::numeric_limits<std::uint32_t>::max()) {
                    throw StartupError{"Networked client ticks are exhausted"};
                }
                const auto playing = client.session_mode() == dots::protocol::SessionMode::Playing;
                auto movement = Vector2{};
                if (playing) {
                    controlled = client.world().find(client.primary_entity_id());
                    if (controlled == nullptr) {
                        throw StartupError{"The replicated local player disappeared"};
                    }
                    const auto predicted_primary = client.predicted_primary_entity_id();
                    const auto predicted_mass =
                        client.predicted_world() != nullptr && predicted_primary.is_valid()
                            ? client.predicted_world()->mass(
                                  dots::simulation::EntityId{predicted_primary.value()})
                            : std::nullopt;
                    viewport.player_radius_pixels = dots::simulation::radius_for_mass(
                                                        predicted_mass.value_or(controlled->mass)) *
                                                    config.view.pixels_per_world_unit;
                    movement = movement_from_input(
                        input, config.controls, viewport, mouse_input_available);
                }
                if (playing && mycore::math::length_squared(movement) > 0.0F) {
                    last_nonzero_movement_input = movement;
                }
                auto action_bits = std::uint16_t{};
                if (playing && split_request_pending) {
                    action_bits |= dots::protocol::kSplitActionBit;
                }
                if (!playing && respawn_request_pending) {
                    action_bits |= dots::protocol::kRespawnActionBit;
                }
                if (client.send_input(client_tick++, movement, action_bits) !=
                    dots::client_runtime::InputSendResult::Sent) {
                    throw StartupError{"The networked client could not send input"};
                }
                if ((action_bits & dots::protocol::kSplitActionBit) != 0U) {
                    split_request_pending = false;
                }
                if ((action_bits & dots::protocol::kRespawnActionBit) != 0U) {
                    respawn_request_pending = false;
                }
                if (embedded_server != nullptr &&
                    (embedded_server->process_events() || embedded_server->step())) {
                    throw StartupError{"The embedded authoritative session failed"};
                }
                if (process_client_events(now)) {
                    throw StartupError{"The networked authoritative session failed"};
                }
                if (client.session_mode() == dots::protocol::SessionMode::Playing) {
                    controlled = client.world().find(client.primary_entity_id());
                    if (controlled == nullptr) {
                        throw StartupError{"The replicated local player disappeared"};
                    }
                    viewport.player_radius_pixels =
                        dots::simulation::radius_for_mass(controlled->mass) *
                        config.view.pixels_per_world_unit;
                } else {
                    controlled = nullptr;
                    split_request_pending = false;
                }
            }
        }
        const auto simulation_duration = std::chrono::duration_cast<mycore::time::Duration>(
            std::chrono::steady_clock::now() - simulation_start);
        simulation_metrics.add_sample({
            .frame_duration = frame_duration,
            .simulation_duration = simulation_duration,
            .backlog = accumulator.accumulated_time(),
            .discarded_time = discarded_frame_time + discarded_backlog,
            .steps = step_result.steps,
            .pending_steps = step_result.pending_steps,
            .step_limit_reached = step_result.step_limit_reached,
        });
        const auto simulation_snapshot = simulation_metrics.snapshot();
        simulation_health_reporter.update(simulation_snapshot, std::chrono::steady_clock::now());
        if (!render_surface_available) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        remote_snapshot_buffer.advance(now);
        const auto playing = client.session_mode() == dots::protocol::SessionMode::Playing;
        controlled = playing ? client.world().find(client.primary_entity_id()) : nullptr;
        if (playing && controlled == nullptr) {
            throw StartupError{"The replicated local player disappeared"};
        }
        if (playing) {
            respawn_request_pending = false;
        } else {
            split_request_pending = false;
        }
        const auto sampled_local_entity =
            playing ? client.primary_entity_id() : dots::protocol::EntityId{};
        const auto remote_frame = remote_snapshot_buffer.sample(sampled_local_entity);
        const auto extrapolated_remote_frame =
            remote_extrapolation_buffer.sample(now, sampled_local_entity);
        const auto remote_presentation_statistics = remote_snapshot_buffer.statistics(now);
        const auto remote_extrapolation_statistics = remote_extrapolation_buffer.statistics(now);
        auto prediction_statistics = client.prediction_statistics(now);
        prediction_debug_controls.observe_input_drop_burst(prediction_statistics, now);
        const auto update_local_prediction_presentation = [&] {
            const auto predicted_position = client.predicted_position();
            if (!predicted_position) {
                throw StartupError{"The local player prediction disappeared"};
            }
            local_prediction_presentation.update(
                {
                    .predicted_position = *predicted_position,
                    .accumulated_correction_displacement =
                        prediction_statistics.accumulated_correction_displacement,
                    .correction_sequence =
                        prediction_statistics.correction_sequence_since_hard_resync,
                    .hard_resync_sequence = prediction_statistics.hard_resync_count,
                    .pre_correction_position = client.pre_correction_position(),
                    .correction_replay_path = client.latest_correction_replay_path(),
                },
                now);
            prediction_correction_samples.clear();
            prediction_correction_samples.reserve(client.recent_prediction_corrections().size());
            for (const auto& correction : client.recent_prediction_corrections()) {
                prediction_correction_samples.push_back({
                    .sequence = correction.sequence,
                    .entity_id = correction.entity_id,
                    .pre_correction_position = correction.pre_correction_position,
                    .mass = correction.mass,
                    .remote = correction.source ==
                              dots::client_runtime::PredictionCorrectionSource::Remote,
                });
            }
            prediction_correction_history.update(
                prediction_correction_samples, prediction_statistics.hard_resync_count, now);
        };
        if (playing) {
            if (spectator_camera_active) {
                local_prediction_presentation.reset();
                spectator_camera_active = false;
            }
            update_local_prediction_presentation();
            last_camera_position = local_prediction_presentation.presentation_position();
        } else {
            if (!spectator_camera_active) {
                spectator_camera.enter(last_camera_position,
                                       render_settings.pixels_per_world_unit,
                                       client.follow_entity_id(),
                                       remote_frame);
                spectator_camera_active = true;
                local_prediction_presentation.reset();
                prediction_correction_history.clear();
                persistent_world_presentation.reset();
            }
            spectator_camera.update(remote_frame,
                                    client.follow_entity_id(),
                                    {
                                        .pan = spectator_control_intent.pan,
                                        .zoom_steps = spectator_control_intent.zoom_steps,
                                        .toggle_follow = spectator_control_intent.toggle_follow,
                                    },
                                    std::chrono::duration<float>{elapsed}.count());
            last_camera_position = spectator_camera.position();
            prediction_debug_controls.requested_prediction_error.reset();
            prediction_debug_controls.drop_input_packets_requested = false;
            prediction_debug_controls.clear_correction_visuals_requested = false;
        }
        const auto representative = std::find_if(
            remote_frame.entities.begin(), remote_frame.entities.end(), [](const auto& entity) {
                return entity.kind == dots::protocol::EntityKind::Player;
            });
        const auto representative_remote_entity = representative == remote_frame.entities.end()
                                                      ? std::nullopt
                                                      : std::optional{representative->entity_id};
        const auto representative_remote_endpoints =
            representative_remote_entity
                ? remote_snapshot_buffer.endpoints(*representative_remote_entity)
                : dots::presentation::RemoteEntityEndpoints{};
        const auto replication_statistics = client.replication_statistics(now);
        auto respawn_seconds_remaining = std::optional<double>{};
        if (const auto deadline = client.respawn_available_tick();
            deadline && replication_statistics.latest_snapshot_age) {
            const auto estimated_server_tick =
                static_cast<double>(client.world().server_tick()) +
                (std::chrono::duration<double>{*replication_statistics.latest_snapshot_age}
                     .count() *
                 static_cast<double>(dots::simulation::kTickRateHz));
            respawn_seconds_remaining =
                std::max(0.0,
                         (static_cast<double>(*deadline) - estimated_server_tick) /
                             static_cast<double>(dots::simulation::kTickRateHz));
        }
        if (config.debug.enabled) {
            draw_debug_overlay(
                config,
                {
                    .presentation = playing ? "NETWORKED PREDICTED" : "NETWORKED SPECTATOR",
                    .tick = client.world().server_tick(),
                    .player_count = client.world().player_count(),
                    .food_count = client.world().food_count(),
                    .current_player_position = controlled == nullptr
                                                   ? std::nullopt
                                                   : std::optional{Vector2{controlled->position_x,
                                                                           controlled->position_y}},
                    .occupied_grid_cells = std::nullopt,
                    .snapshot_id = client.world().snapshot_id().value(),
                    .transport = endpoint.statistics(client.connection_handle()),
                    .replication = replication_statistics,
                    .network_session =
                        DebugWorldStats::NetworkSession{
                            .runtime_state = client.state(),
                            .client_id = client.client_id(),
                            .controlled_entity_id = client.primary_entity_id(),
                            .local_prediction_available = playing,
                            .connection_handle = client.connection_handle(),
                            .server_tick = client.world().server_tick(),
                            .local_input_tick = client_tick,
                            .prediction = prediction_statistics,
                            .remote_presentation = remote_presentation_statistics,
                            .remote_extrapolation = remote_extrapolation_statistics,
                            .consequences = consequence_presentation.statistics(),
                            .persistent_presentation = persistent_world_presentation.statistics(),
                            .remote_presentation_mode = config.debug.remote_presentation_mode,
                            .representative_remote_entity = representative_remote_entity,
                            .representative_remote_endpoints = representative_remote_endpoints,
                            .latest_authoritative_sample =
                                controlled == nullptr
                                    ? std::nullopt
                                    : std::optional{Vector2{controlled->position_x,
                                                            controlled->position_y}},
                            .predicted_position = playing
                                                      ? std::optional{local_prediction_presentation
                                                                          .predicted_position()}
                                                      : std::nullopt,
                            .presentation_position =
                                playing ? std::optional{local_prediction_presentation
                                                            .presentation_position()}
                                        : std::nullopt,
                            .smoothing_offset = playing
                                                    ? std::optional{local_prediction_presentation
                                                                        .smoothing_offset()}
                                                    : std::nullopt,
                            .last_nonzero_movement_input = last_nonzero_movement_input,
                            .retained_correction_count = prediction_correction_history.size(),
                            .correction_history_capacity = prediction_correction_history.capacity(),
                            .retained_local_correction_count =
                                prediction_correction_history.local_count(),
                            .retained_remote_correction_count =
                                prediction_correction_history.remote_count(),
                        },
                    .gameplay_session =
                        DebugWorldStats::GameplaySession{
                            .client_id = client.client_id(),
                            .mode = client.session_mode(),
                            .owned_piece_count = client.owned_entity_ids().size(),
                            .primary_entity_id = client.primary_entity_id(),
                            .follow_entity_id = client.follow_entity_id(),
                            .defeat_tick = client.defeat_tick(),
                            .respawn_available_tick = client.respawn_available_tick(),
                            .respawn_seconds_remaining = respawn_seconds_remaining,
                            .latest_absorption = client.latest_absorption(),
                            .latest_respawn_request_id = client.latest_respawn_request_id(),
                            .latest_respawn_result = client.latest_respawn_result(),
                        },
                },
                frame_metrics.snapshot(),
                simulation_snapshot,
                &prediction_debug_controls);
        }

        if (playing && prediction_debug_controls.requested_prediction_error) {
            if (!client.debug_inject_prediction_error(
                    *prediction_debug_controls.requested_prediction_error)) {
                throw StartupError{"Could not inject the requested prediction error"};
            }
            prediction_statistics = client.prediction_statistics(now);
            update_local_prediction_presentation();
            prediction_debug_controls.requested_prediction_error.reset();
        }
        if (playing && prediction_debug_controls.drop_input_packets_requested) {
            if (!client.debug_drop_next_input_packets(kInjectedInputDropBurstSize)) {
                throw StartupError{"Could not arm the requested input packet drops"};
            }
            prediction_debug_controls.begin_input_drop_burst(
                prediction_statistics.injected_input_drop_count);
            prediction_debug_controls.drop_input_packets_requested = false;
        }
        if (playing && prediction_debug_controls.clear_correction_visuals_requested) {
            local_prediction_presentation.clear_correction_visuals();
            prediction_correction_history.clear();
            prediction_debug_controls.clear_correction_visuals_requested = false;
        }

        std::vector<dots::presentation::RemoteEntityEndpoints> remote_endpoint_layers;
        if (config.debug.enabled && prediction_debug_controls.show_remote_endpoint_layers) {
            remote_endpoint_layers.reserve(remote_frame.entities.size());
            for (const auto& entity : remote_frame.entities) {
                if (entity.kind == dots::protocol::EntityKind::Player) {
                    remote_endpoint_layers.push_back(
                        remote_snapshot_buffer.endpoints(entity.entity_id));
                }
            }
        }
        auto current_render_settings = render_settings;
        auto frame = dots::presentation::FrameData{};
        if (playing) {
            const auto correction_visual_active =
                local_prediction_presentation.correction_visual_active();
            last_camera_position = local_prediction_presentation.presentation_position();
            const auto* predicted_world = client.predicted_world();
            if (predicted_world == nullptr) {
                throw StartupError{"The complete local prediction World disappeared"};
            }
            const auto predicted_primary = client.predicted_primary_entity_id().is_valid()
                                               ? client.predicted_primary_entity_id()
                                               : client.primary_entity_id();
            const auto controlled_presentation = dots::presentation::PredictedReplicatedPlayer{
                .entity_id = predicted_primary,
                .presentation_position = local_prediction_presentation.presentation_position(),
                .predicted_position = local_prediction_presentation.predicted_position(),
                .pre_correction_position = std::nullopt,
                .correction_replay_path =
                    correction_visual_active
                        ? local_prediction_presentation.retained_correction_replay_path()
                        : std::span<const Vector2>{},
                .correction_ghosts = prediction_correction_history.ghosts(),
                .show_prediction_layers =
                    config.debug.enabled && prediction_debug_controls.show_prediction_layers,
                .show_replay_path =
                    config.debug.enabled && prediction_debug_controls.show_replay_path,
            };
            if (config.debug.remote_presentation_mode == RemotePresentationMode::Interpolated ||
                !extrapolated_remote_frame.ready) {
                frame = dots::presentation::extract_remote_interpolated_predicted_frame(
                    client.world(),
                    *predicted_world,
                    client.predicted_scope_entity_ids(),
                    remote_frame,
                    remote_endpoint_layers,
                    controlled_presentation);
            } else {
                frame = dots::presentation::extract_remote_extrapolated_predicted_frame(
                    client.world(),
                    *predicted_world,
                    client.predicted_scope_entity_ids(),
                    extrapolated_remote_frame,
                    remote_endpoint_layers,
                    controlled_presentation);
                if (config.debug.remote_presentation_mode == RemotePresentationMode::Comparison) {
                    dots::presentation::append_interpolated_remote_comparison(
                        frame, remote_frame, client.predicted_scope_entity_ids());
                }
            }
            const auto fixed_tick_alpha =
                std::clamp(static_cast<float>(accumulator.accumulated_time().count()) /
                               static_cast<float>(accumulator.step_duration().count()),
                           0.0F,
                           1.0F);
            frame = persistent_world_presentation.compose(frame,
                                                          fixed_tick_alpha,
                                                          prediction_statistics.hard_resync_count,
                                                          predicted_primary,
                                                          now);
        } else {
            current_render_settings.pixels_per_world_unit =
                spectator_camera.pixels_per_world_unit();
            frame = dots::presentation::extract_remote_interpolated_spectator_frame(
                remote_frame, remote_endpoint_layers, spectator_camera.position());
        }
        consequence_presentation.append_cues(frame, now);
        draw_confirmed_notice(consequence_presentation.confirmed_notice(now));
        const auto presented =
            renderer.render(dots::presentation::build_draw_list(frame, current_render_settings),
                            [&debug_ui](mycore::render::CommandList& commands,
                                        const mycore::render::SwapchainTarget& target) {
                                debug_ui.render(commands, target);
                            });
        if (!presented) {
            debug_ui.cancel_frame();
        }
    }
}

int run_in_memory_game(const ClientConfig& config,
                       mycore::platform_sdl::Window& window,
                       mycore::render_2d::Renderer& renderer,
                       mycore::debug_ui::Context& debug_ui,
                       const dots::presentation::Settings& render_settings) {
    dots::simulation::World authoritative_world;
    if (!dots::simulation::spawn_default_food_field(authoritative_world)) {
        throw StartupError{"Could not spawn the authoritative food field"};
    }
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint(), std::move(authoritative_world)};
    auto& endpoint = network.connect_client();
    return run_networked_game(
        config, window, renderer, debug_ui, render_settings, endpoint, &server);
}

int run_native_game(const ClientConfig& config,
                    mycore::platform_sdl::Window& window,
                    mycore::render_2d::Renderer& renderer,
                    mycore::debug_ui::Context& debug_ui,
                    const dots::presentation::Settings& render_settings,
                    const ClientRunOptions& options) {
    const auto address = mycore::net_transport::NetworkAddress::parse(options.server_address);
    if (!address || address->port() == 0) {
        throw StartupError{"Native mode requires a numeric server address with a nonzero port"};
    }
    mycore::net_transport::GameNetworkingSocketsNetwork network{options.impairment};
    auto& endpoint = network.connect(*address);
    constexpr std::uint32_t kMinimumDisconnectDrainMilliseconds = 50;
    constexpr std::uint32_t kDisconnectDrainMarginMilliseconds = 50;
    constexpr std::uint32_t kMaximumDisconnectDrainMilliseconds = 2'000;
    const auto requested_drain_milliseconds =
        static_cast<std::uint64_t>(options.impairment.outgoing_lag_milliseconds) +
        kDisconnectDrainMarginMilliseconds;
    const auto drain_milliseconds =
        std::clamp(requested_drain_milliseconds,
                   static_cast<std::uint64_t>(kMinimumDisconnectDrainMilliseconds),
                   static_cast<std::uint64_t>(kMaximumDisconnectDrainMilliseconds));
    return run_networked_game(config,
                              window,
                              renderer,
                              debug_ui,
                              render_settings,
                              endpoint,
                              nullptr,
                              std::chrono::milliseconds{drain_milliseconds});
}

} // namespace

int run_client(const ClientConfig& config, const ClientRunOptions& options) {
    const auto mode = options.mode;
    mycore::platform_sdl::Runtime runtime;
    mycore::platform_sdl::Window window{{
        .title = config.window.title,
        .width = config.window.width,
        .height = config.window.height,
        .minimum_width = kMinimumWindowWidth,
        .minimum_height = kMinimumWindowHeight,
        .flags = window_flags(config.window),
        .visible = false,
    }};

    if (mode == ClientRunMode::HeadlessSmoke) {
        static_cast<void>(mycore::platform_sdl::poll_input(window));
        return 0;
    }

    const mycore::assets::DirectorySource assets{
        mycore::platform_sdl::application_base_path() / "assets",
    };
    if (mode == ClientRunMode::PackageSmoke) {
        validate_render_assets(assets);
        static_cast<void>(mycore::platform_sdl::poll_input(window));
        return 0;
    }

    mycore::render::Device device{
        window,
        {
            .shader_format = mycore::render::platform_shader_format(),
            .debug_mode = gpu_debug_mode(),
            .vsync = config.window.vsync,
        },
    };
    mycore::render_2d::Renderer renderer{device, assets};
    mycore::debug_ui::Context debug_ui{window, device};
    const auto render_settings = presentation_settings(config);
    const auto presentation_label =
        mode == ClientRunMode::InMemoryGame || mode == ClientRunMode::NativeGame
            ? std::string_view{"NETWORKED PREDICTED"}
            : presentation_mode_name(config.debug.presentation_mode);
    mycore::debug::log_info("dots.client",
                            "Started SDL_GPU renderer '{}' with {}, {} presentation, and {} input",
                            device.driver_name(),
                            config.window.vsync ? "vsync" : "unlocked",
                            presentation_label,
                            input_mode_name(config.controls.mode));
    window.show();

    if (mode == ClientRunMode::InMemoryGame) {
        return run_in_memory_game(config, window, renderer, debug_ui, render_settings);
    }
    if (mode == ClientRunMode::NativeGame) {
        return run_native_game(config, window, renderer, debug_ui, render_settings, options);
    }

    constexpr auto kOfflineScopeEpoch = mycore::rollback::ScopeEpoch{1};
    constexpr auto kOfflineReplayHorizon = mycore::time::TickDelta{64};
    dots::simulation::World initial_world;
    const auto player = initial_world.spawn_player(dots::simulation::PlayerOwnerId{0});
    if (!player) {
        throw dots::client::StartupError{"Could not spawn the local player"};
    }
    if (!dots::simulation::spawn_default_food_field(initial_world)) {
        throw dots::client::StartupError{"Could not spawn the local food field"};
    }
    const auto initial_checkpoint = initial_world.checkpoint();
    auto scope_result = dots::prediction::build_prediction_scope(
        initial_checkpoint,
        {
            .profile = dots::prediction::PredictionProfile::FullReplicated,
            .mechanics = dots::prediction::kCurrentPredictionMechanics,
            .owned_owner_ids = {dots::simulation::PlayerOwnerId{0}},
            .subscribed_event_owner_ids = {dots::simulation::PlayerOwnerId{0}},
            .replay_horizon = kOfflineReplayHorizon,
            .scope_epoch = kOfflineScopeEpoch,
            .coverage = {},
        });
    const auto* offline_scope = std::get_if<dots::prediction::PredictionScope>(&scope_result);
    if (offline_scope == nullptr) {
        throw dots::client::StartupError{"Could not build the offline prediction scope"};
    }
    auto projected_result =
        dots::prediction::project_checkpoint(initial_checkpoint, *offline_scope);
    const auto* projected_checkpoint =
        std::get_if<dots::simulation::WorldCheckpoint>(&projected_result);
    if (projected_checkpoint == nullptr) {
        throw dots::client::StartupError{"Could not project the offline prediction checkpoint"};
    }
    dots::prediction::Timeline timeline{
        dots::prediction::WorldModel{},
        {.capacity = static_cast<std::size_t>(kOfflineReplayHorizon.value())},
    };
    const auto initialized = timeline.initialize(
        {
            .tick = projected_checkpoint->tick,
            .acknowledged_through = std::nullopt,
            .scope_epoch = kOfflineScopeEpoch,
            .checkpoint = *projected_checkpoint,
            .events = {},
        },
        *offline_scope);
    if (!std::holds_alternative<dots::prediction::Commit>(initialized)) {
        throw dots::client::StartupError{"Could not initialize the offline rollback timeline"};
    }
    dots::presentation::RollbackConsequencePresentation consequence_presentation;
    consequence_presentation.set_local_owner(dots::simulation::PlayerOwnerId{0});
    dots::presentation::PersistentWorldPresentation persistent_world_presentation;
    auto offline_hard_resync_sequence = std::uint64_t{};

    mycore::time::FixedStepAccumulator accumulator{dots::simulation::kTickDuration};
    auto previous_time = std::chrono::steady_clock::now();
    const auto initial_position = timeline.state()->position(*player);
    if (!initial_position) {
        throw dots::client::StartupError{"Could not get player position"};
    }
    auto previous_player_position = *initial_position;
    auto current_player_position = previous_player_position;
    std::uint32_t next_command_id{};
    const auto maximum_frame_delta = std::chrono::duration_cast<mycore::time::Duration>(
        std::chrono::milliseconds{config.simulation.max_frame_delta_ms});
    mycore::debug::FrameMetrics frame_metrics;
    mycore::debug::FixedStepMetrics simulation_metrics{dots::simulation::kTickDuration};
    SimulationHealthReporter simulation_health_reporter;
    PlayerControlTracker player_control_tracker;
    bool split_request_pending{};
    MYCORE_PROFILE_THREAD("Dots client");

    while (true) {
        MYCORE_PROFILE_FRAME();
        MYCORE_PROFILE_ZONE("Dots client frame");
        const auto input =
            mycore::platform_sdl::poll_input(window, config.debug.enabled ? &debug_ui : nullptr);
        if (quit_requested(input, config.controls)) {
            return 0;
        }
        split_request_pending = split_request_pending ||
                                player_control_tracker.sample(input, config.controls).request_split;

        const auto output_size = window.pixel_size();
        const auto logical_size = window.size();
        if (output_size.width <= 0 || output_size.height <= 0 || logical_size.width <= 0 ||
            logical_size.height <= 0) {
            previous_time = std::chrono::steady_clock::now();
            continue;
        }

        debug_ui.begin_frame();
        const auto mouse_input_available = !config.debug.enabled || !debug_ui.wants_mouse_capture();
        const auto now = std::chrono::steady_clock::now();
        const auto frame_duration =
            std::chrono::duration_cast<mycore::time::Duration>(now - previous_time);
        frame_metrics.add_sample(frame_duration);
        const auto elapsed = std::min(frame_duration, maximum_frame_delta);
        const auto discarded_frame_time = frame_duration - elapsed;
        previous_time = now;
        const auto step_result =
            accumulator.advance(elapsed, config.simulation.max_steps_per_frame);
        const auto discarded_backlog = step_result.step_limit_reached
                                           ? accumulator.discard_pending_steps()
                                           : mycore::time::Duration::zero();

        const auto player_radius = timeline.state()->radius(*player);
        if (!player_radius) {
            throw dots::client::StartupError{"The local player disappeared from the world"};
        }
        auto viewport = InputViewport{
            .width = static_cast<float>(output_size.width),
            .height = static_cast<float>(output_size.height),
            .mouse_scale_x =
                static_cast<float>(output_size.width) / static_cast<float>(logical_size.width),
            .mouse_scale_y =
                static_cast<float>(output_size.height) / static_cast<float>(logical_size.height),
            .player_radius_pixels = *player_radius * config.view.pixels_per_world_unit,
        };
        const auto simulation_start = std::chrono::steady_clock::now();
        {
            MYCORE_PROFILE_ZONE("Dots simulation steps");
            for (std::size_t step = 0; step < step_result.steps; ++step) {
                if (next_command_id == dots::simulation::InputCommandId::kInvalidValue) {
                    throw dots::client::StartupError{"Local input command IDs are exhausted"};
                }
                const auto command =
                    make_tick_command(input,
                                      config.controls,
                                      dots::simulation::PlayerOwnerId{0},
                                      dots::simulation::InputCommandId{next_command_id},
                                      viewport,
                                      mouse_input_available,
                                      split_request_pending);
                split_request_pending = false;
                previous_player_position = current_player_position;
                const auto sequence = mycore::rollback::CommandSequence{next_command_id};
                ++next_command_id;
                auto advanced = timeline.advance(sequence,
                                                 dots::prediction::TickStimulus{
                                                     .commands = {command},
                                                     .remote_movement_assumptions = {},
                                                 });
                auto* advanced_commit = std::get_if<dots::prediction::Commit>(&advanced);
                if (advanced_commit == nullptr) {
                    throw dots::client::StartupError{
                        "The offline rollback timeline rejected an atomic tick"};
                }
                const auto advanced_report = consequence_presentation.consume(
                    mycore::rollback::event_batch_from_commit(std::move(*advanced_commit)), now);
                if (!advanced_report.failures.empty()) {
                    mycore::debug::log_error(
                        "dots.client.consequence",
                        "Offline rollback consequence batch reported {} non-retried failures",
                        advanced_report.failures.size());
                }
                if (timeline.history().size() ==
                    static_cast<std::size_t>(kOfflineReplayHorizon.value())) {
                    std::vector<dots::prediction::AuthorityEvent> confirmed_events;
                    for (const auto& frame_record : timeline.history()) {
                        for (const auto& event : frame_record.events) {
                            confirmed_events.push_back({
                                .disposition =
                                    mycore::rollback::AuthorityEventDisposition::Confirmed,
                                .key = dots::simulation::simulation_event_key(event),
                                .event = event,
                            });
                        }
                    }
                    const auto rebased_checkpoint = timeline.state()->checkpoint();
                    auto rebased = timeline.hard_resync(
                        {
                            .tick = rebased_checkpoint.tick,
                            .acknowledged_through = sequence,
                            .scope_epoch = kOfflineScopeEpoch,
                            .checkpoint = rebased_checkpoint,
                            .events = std::move(confirmed_events),
                        },
                        *offline_scope);
                    auto* rebased_commit = std::get_if<dots::prediction::Commit>(&rebased);
                    if (rebased_commit == nullptr) {
                        throw dots::client::StartupError{
                            "Could not prune the offline rollback history"};
                    }
                    ++offline_hard_resync_sequence;
                    const auto rebased_report = consequence_presentation.consume(
                        mycore::rollback::event_batch_from_commit(std::move(*rebased_commit)), now);
                    if (!rebased_report.failures.empty()) {
                        mycore::debug::log_error(
                            "dots.client.consequence",
                            "Offline confirmation batch reported {} non-retried failures",
                            rebased_report.failures.size());
                    }
                }
                const auto position = timeline.state()->position(*player);
                if (!position) {
                    throw dots::client::StartupError{"The local player disappeared from the world"};
                }
                current_player_position = *position;
                const auto current_radius = timeline.state()->radius(*player);
                if (!current_radius) {
                    throw dots::client::StartupError{"The local player disappeared from the world"};
                }
                viewport.player_radius_pixels = *current_radius * config.view.pixels_per_world_unit;
            }
        }
        const auto simulation_duration = std::chrono::duration_cast<mycore::time::Duration>(
            std::chrono::steady_clock::now() - simulation_start);
        simulation_metrics.add_sample({
            .frame_duration = frame_duration,
            .simulation_duration = simulation_duration,
            .backlog = accumulator.accumulated_time(),
            .discarded_time = discarded_frame_time + discarded_backlog,
            .steps = step_result.steps,
            .pending_steps = step_result.pending_steps,
            .step_limit_reached = step_result.step_limit_reached,
        });
        const auto simulation_snapshot = simulation_metrics.snapshot();
        simulation_health_reporter.update(simulation_snapshot, std::chrono::steady_clock::now());

        const auto alpha = std::clamp(static_cast<float>(accumulator.accumulated_time().count()) /
                                          static_cast<float>(accumulator.step_duration().count()),
                                      0.0F,
                                      1.0F);
        dots::presentation::FrameData frame;
        {
            MYCORE_PROFILE_ZONE("Dots presentation extraction");
            frame = dots::presentation::extract_interpolated_follow_frame(
                *timeline.state(),
                {
                    .entity_id = *player,
                    .previous_position = previous_player_position,
                    .current_position = current_player_position,
                    .alpha =
                        config.debug.presentation_mode == PresentationMode::Fixed ? 1.0F : alpha,
                    .show_current_position_ghost =
                        config.debug.enabled &&
                        config.debug.presentation_mode == PresentationMode::Comparison,
                });
            frame = persistent_world_presentation.compose(
                frame,
                config.debug.presentation_mode == PresentationMode::Fixed ? 1.0F : alpha,
                offline_hard_resync_sequence,
                dots::protocol::EntityId{player->value()},
                now);
            consequence_presentation.append_cues(frame, now);
        }

        if (config.debug.enabled) {
            draw_debug_overlay(
                config,
                {
                    .presentation = presentation_mode_name(config.debug.presentation_mode),
                    .tick = timeline.state()->tick().value(),
                    .player_count = timeline.state()->player_count(),
                    .food_count = timeline.state()->food_count(),
                    .current_player_position = current_player_position,
                    .occupied_grid_cells = timeline.state()->occupied_spatial_cell_count(),
                    .snapshot_id = std::nullopt,
                    .transport = std::nullopt,
                    .replication = std::nullopt,
                    .network_session = std::nullopt,
                    .gameplay_session = std::nullopt,
                },
                frame_metrics.snapshot(),
                simulation_snapshot);
        }
        draw_confirmed_notice(consequence_presentation.confirmed_notice(now));
        bool presented{};
        {
            MYCORE_PROFILE_ZONE("Dots render submission");
            presented = renderer.render(dots::presentation::build_draw_list(frame, render_settings),
                                        [&debug_ui](mycore::render::CommandList& commands,
                                                    const mycore::render::SwapchainTarget& target) {
                                            debug_ui.render(commands, target);
                                        });
        }
        if (!presented) {
            debug_ui.cancel_frame();
        }
    }
}

} // namespace dots::client
