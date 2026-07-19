#include "dots/client/client_app.hpp"

#include "dots/client/controls.hpp"
#include "dots/client_runtime/client_runtime.hpp"
#include "dots/presentation/presentation.hpp"
#include "dots/protocol/codec.hpp"
#include "dots/server/server_runtime.hpp"
#include "dots/simulation/world.hpp"
#include "dots/simulation/world_setup.hpp"
#include "mycore/assets/directory_source.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/debug/metrics.hpp"
#include "mycore/debug/profile.hpp"
#include "mycore/debug_ui/context.hpp"
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
    std::optional<std::size_t> occupied_grid_cells;
    std::optional<std::uint32_t> snapshot_id;
    std::optional<mycore::net_transport::TransportStatistics> transport;
    std::optional<dots::client_runtime::ReplicationStatistics> replication;
    struct NetworkSession {
        dots::client_runtime::State runtime_state{};
        dots::protocol::ClientId client_id;
        dots::protocol::EntityId controlled_entity_id;
        mycore::net_transport::ConnectionHandle connection_handle;
        std::uint32_t server_tick{};
        std::uint32_t local_input_tick{};
        dots::client_runtime::PredictionStatistics prediction;
        Vector2 latest_authoritative_sample;
        Vector2 predicted_position;
        Vector2 presentation_position;
        Vector2 smoothing_offset;
    };
    std::optional<NetworkSession> network_session;
};

struct PredictionDebugControls {
    bool show_prediction_layers{true};
    bool show_replay_path{true};
    bool inject_prediction_error_requested{};
    bool drop_input_packets_requested{};
    bool clear_correction_visuals_requested{};
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

void draw_input_sequence(std::string_view label, dots::protocol::InputSequenceId value) {
    if (value.is_valid()) {
        ImGui::Text("%.*s: %u", static_cast<int>(label.size()), label.data(), value.value());
    } else {
        ImGui::Text("%.*s: none", static_cast<int>(label.size()), label.data());
    }
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
    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - kMargin,
                             viewport->WorkPos.y + viewport->WorkSize.y - kMargin},
                            ImGuiCond_Always,
                            {1.0F, 1.0F});
    ImGui::SetNextWindowSizeConstraints({overlay_width, 0.0F}, {overlay_width, available_height});
    ImGui::SetNextWindowBgAlpha(0.82F);

    constexpr auto kWindowFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Dots observability", nullptr, kWindowFlags)) {
        const auto input_mode = input_mode_name(config.controls.mode);
        ImGui::TextUnformatted("Dots debug");
        ImGui::Separator();
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
        if (world.replication) {
            ImGui::Separator();
            ImGui::TextUnformatted("Replication");
            if (world.replication->latest_snapshot_age) {
                ImGui::Text(
                    "Snapshot age: %lld ms",
                    static_cast<long long>(world.replication->latest_snapshot_age->count()));
            } else {
                ImGui::TextUnformatted("Snapshot age: unavailable");
            }
            ImGui::Text("Receive rate: %.1f snapshots/s",
                        world.replication->accepted_snapshots_per_second);
        }
        if (world.network_session) {
            const auto& session = *world.network_session;
            const auto& prediction = session.prediction;
            ImGui::Separator();
            ImGui::TextUnformatted("Session");
            const auto runtime_state = runtime_state_name(session.runtime_state);
            ImGui::Text(
                "Runtime: %.*s", static_cast<int>(runtime_state.size()), runtime_state.data());
            if (world.transport) {
                const auto connection_state = connection_state_name(world.transport->state);
                ImGui::Text("Connection: %.*s",
                            static_cast<int>(connection_state.size()),
                            connection_state.data());
            } else {
                ImGui::TextUnformatted("Connection: unavailable");
            }
            ImGui::Text("Protocol: %u",
                        static_cast<unsigned int>(dots::protocol::kProtocolVersion));
            ImGui::Text("Client ID: %u", session.client_id.value());
            ImGui::Text("Controlled entity: %u", session.controlled_entity_id.value());
            ImGui::Text("Connection handle: %u", session.connection_handle.value());
            ImGui::Text("Snapshot / server tick: %u / %u",
                        world.snapshot_id.value_or(0),
                        session.server_tick);
            ImGui::Text("Local input tick (next): %u", session.local_input_tick);
            ImGui::TextDisabled("Local and server ticks are not synchronized.");

            ImGui::Separator();
            ImGui::TextUnformatted("Prediction");
            ImGui::Text("Redundancy: %s",
                        prediction.input_redundancy_enabled ? "ENABLED" : "DISABLED");
            draw_input_sequence("Last input sent", prediction.last_input_sent);
            draw_input_sequence("Last input acknowledged", prediction.last_input_acknowledged);
            ImGui::Text("Command lead: %zu", prediction.unacknowledged_input_count);
            const auto history_percent =
                prediction.history_capacity > 0
                    ? (100.0F * static_cast<float>(prediction.history_count)) /
                          static_cast<float>(prediction.history_capacity)
                    : 0.0F;
            ImGui::TextColored(history_utilization_color(history_percent),
                               "History: %zu / %zu (%.1f%%), high %zu",
                               prediction.history_count,
                               prediction.history_capacity,
                               history_percent,
                               prediction.history_high_water_mark);
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
            ImGui::Text("Reconciliations / corrections: %llu / %llu",
                        static_cast<unsigned long long>(prediction.reconciliation_count),
                        static_cast<unsigned long long>(prediction.nonzero_correction_count));
            ImGui::Text("Correction last / max: %.4f / %.4f units",
                        prediction.latest_correction_distance,
                        prediction.maximum_correction_distance);
            ImGui::Text("Corrections/min: %.0f", prediction.corrections_per_minute);
            ImGui::Text("Authority sample: (%.3f, %.3f)",
                        session.latest_authoritative_sample.x,
                        session.latest_authoritative_sample.y);
            ImGui::Text("Predicted: (%.3f, %.3f)",
                        session.predicted_position.x,
                        session.predicted_position.y);
            ImGui::Text("Presentation: (%.3f, %.3f)",
                        session.presentation_position.x,
                        session.presentation_position.y);
            ImGui::Text("Smoothing offset: (%.3f, %.3f), |v| %.4f",
                        session.smoothing_offset.x,
                        session.smoothing_offset.y,
                        mycore::math::length(session.smoothing_offset));
            ImGui::Text("Replay over budget / hard resync: %llu / %llu",
                        static_cast<unsigned long long>(prediction.replay_over_budget_count),
                        static_cast<unsigned long long>(prediction.hard_resync_count));
            ImGui::Text(
                "Injected drops / errors: %llu / %llu",
                static_cast<unsigned long long>(prediction.injected_input_drop_count),
                static_cast<unsigned long long>(prediction.injected_prediction_error_count));

            if (prediction.pending_injected_input_drop_count > 0) {
                ImGui::TextColored({1.0F, 0.25F, 0.2F, 1.0F},
                                   "FAULT ARMED: dropping next %zu input packet(s)",
                                   prediction.pending_injected_input_drop_count);
            }
            if (prediction_controls != nullptr) {
                ImGui::Checkbox("Show prediction layers",
                                &prediction_controls->show_prediction_layers);
                ImGui::Checkbox("Show correction replay", &prediction_controls->show_replay_path);
                if (ImGui::Button("Inject +1 X error")) {
                    prediction_controls->inject_prediction_error_requested = true;
                }
                if (ImGui::Button("Drop next 3 input packets")) {
                    prediction_controls->drop_input_packets_requested = true;
                }
                if (ImGui::Button("Clear correction ghosts")) {
                    prediction_controls->clear_correction_visuals_requested = true;
                }
            }
            ImGui::TextDisabled("Layers: white predicted; orange latest authoritative sample;");
            ImGui::TextDisabled("magenta pre-correction; purple replay; fill is presentation.");
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
            if (world.transport->pending_reliable_bytes &&
                world.transport->pending_unreliable_bytes) {
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
                const auto delay = std::chrono::duration<double, std::milli>{
                    *world.transport->outbound_queue_delay};
                ImGui::Text("Queue delay: %.2f ms", delay.count());
            }
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
    ImGui::End();
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

int run_networked_game(const ClientConfig& config,
                       mycore::platform_sdl::Window& window,
                       mycore::render_2d::Renderer& renderer,
                       mycore::debug_ui::Context& debug_ui,
                       const dots::presentation::Settings& render_settings,
                       mycore::net_transport::Endpoint& endpoint,
                       dots::server::Runtime* embedded_server) {
    using namespace std::chrono_literals;
    dots::client_runtime::Runtime client{
        endpoint,
        {.input_redundancy = config.network.input_redundancy},
    };
    const auto handshake_deadline = std::chrono::steady_clock::now() + 10s;
    while (client.state() != dots::client_runtime::State::Ready) {
        if (client.process_events()) {
            throw StartupError{"The networked client handshake failed"};
        }
        if (embedded_server != nullptr && embedded_server->process_events()) {
            throw StartupError{"The embedded authoritative server handshake failed"};
        }
        if (client.process_events()) {
            throw StartupError{"The networked client handshake failed"};
        }
        if (client.state() == dots::client_runtime::State::Disconnected ||
            client.state() == dots::client_runtime::State::Failed ||
            std::chrono::steady_clock::now() >= handshake_deadline) {
            throw StartupError{"Could not establish the authoritative session"};
        }
        std::this_thread::sleep_for(1ms);
    }

    mycore::time::FixedStepAccumulator accumulator{dots::simulation::kTickDuration};
    auto previous_time = std::chrono::steady_clock::now();
    std::uint32_t client_tick{};
    const auto maximum_frame_delta = std::chrono::duration_cast<mycore::time::Duration>(
        std::chrono::milliseconds{config.simulation.max_frame_delta_ms});
    mycore::debug::FrameMetrics frame_metrics;
    mycore::debug::FixedStepMetrics simulation_metrics{dots::simulation::kTickDuration};
    SimulationHealthReporter simulation_health_reporter;
    dots::presentation::LocalPredictionPresentation local_prediction_presentation;
    PredictionDebugControls prediction_debug_controls;

    while (true) {
        MYCORE_PROFILE_FRAME();
        MYCORE_PROFILE_ZONE("Dots networked client frame");
        const auto input = mycore::platform_sdl::poll_input(window, &debug_ui);
        if (quit_requested(input, config.controls)) {
            if (!client.disconnect()) {
                mycore::debug::log_warning(
                    "dots.client.session",
                    "Could not request a graceful disconnect for connection {}",
                    client.connection_handle().value());
            }
            return 0;
        }

        const auto output_size = window.pixel_size();
        const auto logical_size = window.size();
        if (output_size.width <= 0 || output_size.height <= 0 || logical_size.width <= 0 ||
            logical_size.height <= 0) {
            previous_time = std::chrono::steady_clock::now();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (client.process_events(now)) {
            throw StartupError{"The networked authoritative session failed"};
        }
        if (client.state() != dots::client_runtime::State::Ready) {
            throw StartupError{"The networked authoritative session disconnected"};
        }
        debug_ui.begin_frame();
        const auto mouse_input_available = !debug_ui.wants_mouse_capture();
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

        const auto* controlled = client.world().find(client.controlled_entity_id());
        if (controlled == nullptr) {
            throw StartupError{"The replicated local player disappeared"};
        }
        auto viewport = InputViewport{
            .width = static_cast<float>(output_size.width),
            .height = static_cast<float>(output_size.height),
            .mouse_scale_x =
                static_cast<float>(output_size.width) / static_cast<float>(logical_size.width),
            .mouse_scale_y =
                static_cast<float>(output_size.height) / static_cast<float>(logical_size.height),
            .player_radius_pixels = dots::simulation::radius_for_mass(controlled->mass) *
                                    config.view.pixels_per_world_unit,
        };

        const auto simulation_start = std::chrono::steady_clock::now();
        {
            MYCORE_PROFILE_ZONE("Dots network input steps");
            for (std::size_t step = 0; step < step_result.steps; ++step) {
                if (client_tick == std::numeric_limits<std::uint32_t>::max()) {
                    throw StartupError{"Networked client ticks are exhausted"};
                }
                const auto movement =
                    movement_from_input(input, config.controls, viewport, mouse_input_available);
                if (client.send_input(client_tick++, movement) !=
                    dots::client_runtime::InputSendResult::Sent) {
                    throw StartupError{"The networked client could not send input"};
                }
                if (embedded_server != nullptr &&
                    (embedded_server->process_events() || embedded_server->step())) {
                    throw StartupError{"The embedded authoritative session failed"};
                }
                if (client.process_events(now)) {
                    throw StartupError{"The networked authoritative session failed"};
                }
                controlled = client.world().find(client.controlled_entity_id());
                if (controlled == nullptr) {
                    throw StartupError{"The replicated local player disappeared"};
                }
                viewport.player_radius_pixels =
                    dots::simulation::radius_for_mass(controlled->mass) *
                    config.view.pixels_per_world_unit;
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

        auto prediction_statistics = client.prediction_statistics(now);
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
        };
        update_local_prediction_presentation();
        draw_debug_overlay(
            config,
            {
                .presentation = "NETWORKED PREDICTED",
                .tick = client.world().server_tick(),
                .player_count = client.world().player_count(),
                .food_count = client.world().food_count(),
                .occupied_grid_cells = std::nullopt,
                .snapshot_id = client.world().snapshot_id().value(),
                .transport = endpoint.statistics(client.connection_handle()),
                .replication = client.replication_statistics(now),
                .network_session =
                    DebugWorldStats::NetworkSession{
                        .runtime_state = client.state(),
                        .client_id = client.client_id(),
                        .controlled_entity_id = client.controlled_entity_id(),
                        .connection_handle = client.connection_handle(),
                        .server_tick = client.world().server_tick(),
                        .local_input_tick = client_tick,
                        .prediction = prediction_statistics,
                        .latest_authoritative_sample = {controlled->position_x,
                                                        controlled->position_y},
                        .predicted_position = local_prediction_presentation.predicted_position(),
                        .presentation_position =
                            local_prediction_presentation.presentation_position(),
                        .smoothing_offset = local_prediction_presentation.smoothing_offset(),
                    },
            },
            frame_metrics.snapshot(),
            simulation_snapshot,
            &prediction_debug_controls);

        if (prediction_debug_controls.inject_prediction_error_requested) {
            if (!client.debug_inject_prediction_error({1.0F, 0.0F})) {
                throw StartupError{"Could not inject the requested prediction error"};
            }
            prediction_statistics = client.prediction_statistics(now);
            update_local_prediction_presentation();
            prediction_debug_controls.inject_prediction_error_requested = false;
        }
        if (prediction_debug_controls.drop_input_packets_requested) {
            if (!client.debug_drop_next_input_packets(3)) {
                throw StartupError{"Could not arm the requested input packet drops"};
            }
            prediction_debug_controls.drop_input_packets_requested = false;
        }
        if (prediction_debug_controls.clear_correction_visuals_requested) {
            local_prediction_presentation.clear_correction_visuals();
            prediction_debug_controls.clear_correction_visuals_requested = false;
        }

        const auto correction_visual_active =
            local_prediction_presentation.correction_visual_active();
        const auto frame = dots::presentation::extract_predicted_replicated_frame(
            client.world(),
            {
                .entity_id = client.controlled_entity_id(),
                .presentation_position = local_prediction_presentation.presentation_position(),
                .predicted_position = local_prediction_presentation.predicted_position(),
                .pre_correction_position =
                    correction_visual_active
                        ? local_prediction_presentation.retained_pre_correction_position()
                        : std::nullopt,
                .correction_replay_path =
                    correction_visual_active
                        ? local_prediction_presentation.retained_correction_replay_path()
                        : std::span<const Vector2>{},
                .show_prediction_layers = prediction_debug_controls.show_prediction_layers,
                .show_replay_path = prediction_debug_controls.show_replay_path,
            });
        const auto presented =
            renderer.render(dots::presentation::build_draw_list(frame, render_settings),
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
    return run_networked_game(
        config, window, renderer, debug_ui, render_settings, endpoint, nullptr);
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

    dots::simulation::World world;
    const auto player = world.spawn_player();
    if (!player) {
        throw dots::client::StartupError{"Could not spawn the local player"};
    }
    if (!dots::simulation::spawn_default_food_field(world)) {
        throw dots::client::StartupError{"Could not spawn the local food field"};
    }

    mycore::time::FixedStepAccumulator accumulator{dots::simulation::kTickDuration};
    auto previous_time = std::chrono::steady_clock::now();
    const auto initial_position = world.position(*player);
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
    MYCORE_PROFILE_THREAD("Dots client");

    while (true) {
        MYCORE_PROFILE_FRAME();
        MYCORE_PROFILE_ZONE("Dots client frame");
        const auto input = mycore::platform_sdl::poll_input(window, &debug_ui);
        if (quit_requested(input, config.controls)) {
            return 0;
        }

        const auto output_size = window.pixel_size();
        const auto logical_size = window.size();
        if (output_size.width <= 0 || output_size.height <= 0 || logical_size.width <= 0 ||
            logical_size.height <= 0) {
            previous_time = std::chrono::steady_clock::now();
            continue;
        }

        debug_ui.begin_frame();
        const auto mouse_input_available = !debug_ui.wants_mouse_capture();
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

        const auto player_radius = world.radius(*player);
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
                    make_input_command(input,
                                       config.controls,
                                       *player,
                                       dots::simulation::InputCommandId{next_command_id},
                                       viewport,
                                       mouse_input_available);
                ++next_command_id;
                if (!world.apply_input(command)) {
                    throw dots::client::StartupError{"The local world rejected an input command"};
                }
                previous_player_position = current_player_position;
                if (!world.step()) {
                    throw dots::client::StartupError{"The local world rejected a simulation step"};
                }
                const auto position = world.position(*player);
                if (!position) {
                    throw dots::client::StartupError{"The local player disappeared from the world"};
                }
                current_player_position = *position;
                const auto current_radius = world.radius(*player);
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
                world,
                {
                    .entity_id = *player,
                    .previous_position = previous_player_position,
                    .current_position = current_player_position,
                    .alpha =
                        config.debug.presentation_mode == PresentationMode::Fixed ? 1.0F : alpha,
                    .show_current_position_ghost =
                        config.debug.presentation_mode == PresentationMode::Comparison,
                });
        }

        draw_debug_overlay(
            config,
            {
                .presentation = presentation_mode_name(config.debug.presentation_mode),
                .tick = world.tick().value(),
                .player_count = world.player_count(),
                .food_count = world.food_count(),
                .occupied_grid_cells = world.occupied_spatial_cell_count(),
                .snapshot_id = std::nullopt,
                .transport = std::nullopt,
                .replication = std::nullopt,
                .network_session = std::nullopt,
            },
            frame_metrics.snapshot(),
            simulation_snapshot);
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
