#include "dots/client/client_app.hpp"

#include "dots/client/controls.hpp"
#include "dots/presentation/presentation.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/assets/directory_source.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/debug/metrics.hpp"
#include "mycore/debug/profile.hpp"
#include "mycore/debug_ui/context.hpp"
#include "mycore/math/vector2.hpp"
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
#include <string>

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

void spawn_food_field(dots::simulation::World& world) {
    constexpr float kSpacing = 8.0F;
    for (int row = -6; row <= 6; ++row) {
        for (int column = -10; column <= 10; ++column) {
            if (row == 0 && column == 0) {
                continue;
            }
            const auto food = world.spawn_food(
                {static_cast<float>(column) * kSpacing, static_cast<float>(row) * kSpacing});
            if (!food) {
                throw dots::client::StartupError{"Could not spawn the local food field"};
            }
        }
    }
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

void draw_debug_overlay(const ClientConfig& config,
                        const dots::simulation::World& world,
                        std::size_t simulation_steps,
                        const mycore::debug::FrameMetricsSnapshot& frame_metrics) {
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
        ImGui::Text("Tick: %llu", static_cast<unsigned long long>(world.tick().value()));
        ImGui::Text("Players: %zu", world.player_count());
        ImGui::Text("Food: %zu", world.food_count());
        ImGui::Text("Grid cells: %zu", world.occupied_spatial_cell_count());
        ImGui::Text("Simulation steps: %zu", simulation_steps);
        ImGui::Separator();
        ImGui::Text("Frame: %.2f ms", frame_metrics.latest_milliseconds);
        ImGui::Text("Average: %.2f ms", frame_metrics.average_milliseconds);
        ImGui::Text("FPS: %.1f", frame_metrics.frames_per_second);
    }
    ImGui::End();
}

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
    mycore::debug::log_info("dots.client",
                            "Started SDL_GPU renderer '{}' with {} presentation and {} input",
                            device.driver_name(),
                            config.window.vsync ? "vsync" : "unlocked",
                            input_mode_name(config.controls.mode));
    window.show();

    dots::simulation::World world;
    const auto player = world.spawn_player();
    if (!player) {
        throw dots::client::StartupError{"Could not spawn the local player"};
    }
    spawn_food_field(world);

    mycore::time::FixedStepAccumulator accumulator{dots::simulation::kTickDuration};
    auto previous_time = std::chrono::steady_clock::now();
    auto previous_player_position = *world.position(*player);
    auto current_player_position = previous_player_position;
    std::uint32_t next_command_id{};
    const auto maximum_frame_delta =
        std::chrono::milliseconds{config.simulation.max_frame_delta_ms};
    mycore::debug::FrameMetrics frame_metrics;
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
        const auto frame_duration = now - previous_time;
        frame_metrics.add_sample(
            std::chrono::duration_cast<std::chrono::nanoseconds>(frame_duration));
        const auto elapsed = std::min(
            frame_duration,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(maximum_frame_delta));
        previous_time = now;
        const auto step_result =
            accumulator.advance(std::chrono::duration_cast<mycore::time::Duration>(elapsed),
                                config.simulation.max_steps_per_frame);

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

        const auto alpha = std::clamp(static_cast<float>(step_result.remainder.count()) /
                                          static_cast<float>(accumulator.step_duration().count()),
                                      0.0F,
                                      1.0F);
        const auto camera = previous_player_position +
                            ((current_player_position - previous_player_position) * alpha);
        dots::presentation::FrameData frame;
        {
            MYCORE_PROFILE_ZONE("Dots presentation extraction");
            frame = dots::presentation::extract_frame(world, camera);
        }

        debug_ui.begin_frame();
        draw_debug_overlay(config, world, step_result.steps, frame_metrics.snapshot());
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
