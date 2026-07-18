#include "dots/client/client_app.hpp"

#include "dots/client/controls.hpp"
#include "dots/client_runtime/client_runtime.hpp"
#include "dots/presentation/presentation.hpp"
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
#include <string>
#include <string_view>
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
};

void draw_debug_overlay(const ClientConfig& config,
                        const DebugWorldStats& world,
                        const mycore::debug::FrameMetricsSnapshot& frame_metrics,
                        const mycore::debug::FixedStepMetricsSnapshot& simulation_metrics) {
    constexpr float kMargin = 12.0F;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - kMargin,
                             viewport->WorkPos.y + viewport->WorkSize.y - kMargin},
                            ImGuiCond_Always,
                            {1.0F, 1.0F});
    ImGui::SetNextWindowBgAlpha(0.82F);

    constexpr auto kWindowFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
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
    dots::client_runtime::Runtime client{network.connect_client()};
    if (client.process_events() || server.process_events() || client.process_events() ||
        client.state() != dots::client_runtime::State::Ready) {
        throw StartupError{"Could not establish the in-memory authoritative session"};
    }

    mycore::time::FixedStepAccumulator accumulator{dots::simulation::kTickDuration};
    auto previous_time = std::chrono::steady_clock::now();
    std::uint32_t client_tick{};
    const auto maximum_frame_delta = std::chrono::duration_cast<mycore::time::Duration>(
        std::chrono::milliseconds{config.simulation.max_frame_delta_ms});
    mycore::debug::FrameMetrics frame_metrics;
    mycore::debug::FixedStepMetrics simulation_metrics{dots::simulation::kTickDuration};
    SimulationHealthReporter simulation_health_reporter;

    while (true) {
        MYCORE_PROFILE_FRAME();
        MYCORE_PROFILE_ZONE("Dots in-memory client frame");
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
            MYCORE_PROFILE_ZONE("Dots authoritative in-memory steps");
            for (std::size_t step = 0; step < step_result.steps; ++step) {
                if (client_tick == std::numeric_limits<std::uint32_t>::max()) {
                    throw StartupError{"In-memory client ticks are exhausted"};
                }
                const auto movement = movement_from_input(input, config.controls, viewport);
                if (client.send_input(client_tick++, movement) !=
                    dots::client_runtime::InputSendResult::Sent) {
                    throw StartupError{"The in-memory client could not send input"};
                }
                if (server.process_events() || server.step() || client.process_events()) {
                    throw StartupError{"The in-memory authoritative session failed"};
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

        const auto frame = dots::presentation::extract_replicated_frame(
            client.world(), client.controlled_entity_id());
        debug_ui.begin_frame();
        draw_debug_overlay(config,
                           {
                               .presentation = "NETWORKED FIXED",
                               .tick = client.world().server_tick(),
                               .player_count = client.world().player_count(),
                               .food_count = client.world().food_count(),
                               .snapshot_id = client.world().snapshot_id().value(),
                           },
                           frame_metrics.snapshot(),
                           simulation_snapshot);
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

} // namespace

int run_client(const ClientConfig& config, ClientRunMode mode) {
    mycore::platform_sdl::Runtime runtime;
    mycore::platform_sdl::Window window{{
        .title = config.window.title,
        .width = config.window.width,
        .height = config.window.height,
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
    const auto presentation_label = mode == ClientRunMode::InMemoryGame
                                        ? std::string_view{"NETWORKED FIXED"}
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
                                       viewport);
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

        debug_ui.begin_frame();
        draw_debug_overlay(
            config,
            {
                .presentation = presentation_mode_name(config.debug.presentation_mode),
                .tick = world.tick().value(),
                .player_count = world.player_count(),
                .food_count = world.food_count(),
                .occupied_grid_cells = world.occupied_spatial_cell_count(),
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
