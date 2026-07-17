#include "dots/client/client_app.hpp"

#include "dots/client/controls.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/platform_sdl/error.hpp"
#include "mycore/platform_sdl/input.hpp"
#include "mycore/platform_sdl/runtime.hpp"
#include "mycore/platform_sdl/window.hpp"
#include "mycore/time/time.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dots::client {
namespace {

using mycore::math::Vector2;
using PlatformStartupError = mycore::platform_sdl::StartupError;

struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const noexcept {
        SDL_DestroyRenderer(renderer);
    }
};

using Renderer = std::unique_ptr<SDL_Renderer, RendererDeleter>;

void require_sdl(bool result, std::string_view operation) {
    if (!result) {
        throw PlatformStartupError::from_sdl(operation);
    }
}

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

void set_color(SDL_Renderer* renderer, RgbColor color) {
    require_sdl(SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255),
                "Could not set the SDL render color");
}

void draw_line(SDL_Renderer* renderer, float x1, float y1, float x2, float y2) {
    require_sdl(SDL_RenderLine(renderer, x1, y1, x2, y2), "Could not draw an SDL line");
}

void draw_grid(SDL_Renderer* renderer,
               int output_width,
               int output_height,
               Vector2 camera,
               const ClientConfig& config) {
    if (!config.view.draw_grid) {
        return;
    }

    const auto spacing = config.view.grid_spacing_world_units * config.view.pixels_per_world_unit;
    if (!std::isfinite(spacing) || spacing < 4.0F) {
        return;
    }

    set_color(renderer, config.colors.grid);
    const auto width = static_cast<float>(output_width);
    const auto height = static_cast<float>(output_height);
    auto first_x =
        std::fmod((width * 0.5F) - (camera.x * config.view.pixels_per_world_unit), spacing);
    auto first_y =
        std::fmod((height * 0.5F) - (camera.y * config.view.pixels_per_world_unit), spacing);
    if (first_x < 0.0F) {
        first_x += spacing;
    }
    if (first_y < 0.0F) {
        first_y += spacing;
    }
    for (auto x = first_x; x <= width; x += spacing) {
        draw_line(renderer, x, 0.0F, x, height);
    }
    for (auto y = first_y; y <= height; y += spacing) {
        draw_line(renderer, 0.0F, y, width, y);
    }
}

Vector2 world_to_screen(Vector2 position,
                        Vector2 camera,
                        int output_width,
                        int output_height,
                        float pixels_per_world_unit) {
    return {
        ((position.x - camera.x) * pixels_per_world_unit) +
            (static_cast<float>(output_width) * 0.5F),
        ((position.y - camera.y) * pixels_per_world_unit) +
            (static_cast<float>(output_height) * 0.5F),
    };
}

void draw_filled_circle(
    SDL_Renderer* renderer, Vector2 center, float radius, int output_width, int output_height) {
    if (radius <= 0.0F || output_width <= 0 || output_height <= 0 || !std::isfinite(center.x) ||
        !std::isfinite(center.y)) {
        return;
    }

    const auto center_x = static_cast<double>(center.x);
    const auto center_y = static_cast<double>(center.y);
    const auto double_radius = static_cast<double>(radius);
    const auto top = center_y - double_radius;
    const auto bottom = center_y + double_radius;
    if (bottom < 0.0 || top > static_cast<double>(output_height - 1)) {
        return;
    }

    const auto first_row = top <= 0.0 ? 0 : static_cast<int>(std::ceil(top));
    const auto last_row = bottom >= static_cast<double>(output_height - 1)
                              ? output_height - 1
                              : static_cast<int>(std::floor(bottom));
    for (auto row = first_row; row <= last_row; ++row) {
        const auto y_offset = static_cast<double>(row) - center_y;
        const auto half_width =
            std::sqrt(std::max(0.0, (double_radius * double_radius) - (y_offset * y_offset)));
        const auto left = std::max(0.0, center_x - half_width);
        const auto right = std::min(static_cast<double>(output_width - 1), center_x + half_width);
        if (left > right) {
            continue;
        }
        draw_line(renderer,
                  static_cast<float>(left),
                  static_cast<float>(row),
                  static_cast<float>(right),
                  static_cast<float>(row));
    }
}

void draw_input_mode_hud(SDL_Renderer* renderer,
                         int output_width,
                         int output_height,
                         const ClientConfig& config) {
    constexpr float kHudScale = 2.0F;
    constexpr float kGlyphWidth = 8.0F;
    constexpr float kPadding = 6.0F;
    constexpr float kMargin = 6.0F;
    constexpr float kPanelHeight = 8.0F + (kPadding * 2.0F);
    const auto mode = input_mode_name(config.controls.mode);
    const auto label = std::string{"INPUT: "} + std::string{mode};
    const auto panel_width = (static_cast<float>(label.size()) * kGlyphWidth) + (kPadding * 2.0F);
    const auto logical_width = static_cast<float>(output_width) / kHudScale;
    const auto logical_height = static_cast<float>(output_height) / kHudScale;
    const auto panel_x = std::max(0.0F, logical_width - panel_width - kMargin);
    const auto panel_y = std::max(0.0F, logical_height - kPanelHeight - kMargin);
    const SDL_FRect panel{
        .x = panel_x,
        .y = panel_y,
        .w = panel_width,
        .h = kPanelHeight,
    };

    require_sdl(SDL_SetRenderScale(renderer, kHudScale, kHudScale),
                "Could not set the SDL HUD scale");
    set_color(renderer, config.colors.grid);
    require_sdl(SDL_RenderFillRect(renderer, &panel), "Could not draw the SDL HUD panel");
    set_color(renderer, config.colors.player);
    require_sdl(
        SDL_RenderDebugText(renderer, panel_x + kPadding, panel_y + kPadding, label.c_str()),
        "Could not draw the SDL input-mode HUD");
    require_sdl(SDL_SetRenderScale(renderer, 1.0F, 1.0F), "Could not restore the SDL render scale");
}

std::vector<dots::simulation::EntityId> spawn_food_field(dots::simulation::World& world) {
    constexpr float kSpacing = 8.0F;
    std::vector<dots::simulation::EntityId> food_ids;
    food_ids.reserve(272);
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
            food_ids.push_back(*food);
        }
    }
    return food_ids;
}

void render_world(SDL_Renderer* renderer,
                  int output_width,
                  int output_height,
                  Vector2 camera,
                  const ClientConfig& config,
                  const dots::simulation::World& world,
                  dots::simulation::EntityId player_id,
                  const std::vector<dots::simulation::EntityId>& food_ids) {
    set_color(renderer, config.colors.background);
    require_sdl(SDL_RenderClear(renderer), "Could not clear the SDL renderer");
    draw_grid(renderer, output_width, output_height, camera, config);

    set_color(renderer, config.colors.food);
    for (const auto food_id : food_ids) {
        const auto position = world.position(food_id);
        const auto radius = world.radius(food_id);
        if (!position || !radius) {
            continue;
        }
        draw_filled_circle(
            renderer,
            world_to_screen(
                *position, camera, output_width, output_height, config.view.pixels_per_world_unit),
            *radius * config.view.pixels_per_world_unit,
            output_width,
            output_height);
    }

    const auto player_position = world.position(player_id);
    const auto player_radius = world.radius(player_id);
    if (!player_position || !player_radius) {
        throw dots::client::StartupError{"The local player disappeared from the world"};
    }
    set_color(renderer, config.colors.player);
    draw_filled_circle(renderer,
                       world_to_screen(*player_position,
                                       camera,
                                       output_width,
                                       output_height,
                                       config.view.pixels_per_world_unit),
                       *player_radius * config.view.pixels_per_world_unit,
                       output_width,
                       output_height);

    draw_input_mode_hud(renderer, output_width, output_height, config);

    require_sdl(SDL_RenderPresent(renderer), "Could not present the SDL renderer");
}

} // namespace

int run_client(const ClientConfig& config, bool headless_smoke) {
    mycore::platform_sdl::Runtime runtime;
    mycore::platform_sdl::Window window{{
        .title = config.window.title,
        .width = config.window.width,
        .height = config.window.height,
        .flags = window_flags(config.window),
        .visible = false,
    }};

    if (headless_smoke) {
        static_cast<void>(mycore::platform_sdl::poll_input(window));
        return 0;
    }

    Renderer renderer{SDL_CreateRenderer(window.native_handle(), nullptr)};
    if (!renderer) {
        throw PlatformStartupError::from_sdl("Could not create the temporary SDL renderer");
    }
    require_sdl(SDL_SetRenderVSync(renderer.get(), config.window.vsync ? 1 : 0),
                "Could not configure SDL renderer vsync");
    window.show();

    dots::simulation::World world;
    const auto player = world.spawn_player();
    if (!player) {
        throw dots::client::StartupError{"Could not spawn the local player"};
    }
    const auto food_ids = spawn_food_field(world);

    mycore::time::FixedStepAccumulator accumulator{dots::simulation::kTickDuration};
    auto previous_time = std::chrono::steady_clock::now();
    auto previous_player_position = *world.position(*player);
    auto current_player_position = previous_player_position;
    std::uint32_t next_command_id{};
    const auto maximum_frame_delta =
        std::chrono::milliseconds{config.simulation.max_frame_delta_ms};

    while (true) {
        const auto input = mycore::platform_sdl::poll_input(window);
        if (quit_requested(input, config.controls)) {
            return 0;
        }

        int output_width{};
        int output_height{};
        require_sdl(SDL_GetRenderOutputSize(renderer.get(), &output_width, &output_height),
                    "Could not query the SDL renderer output size");
        const auto logical_size = window.size();
        if (output_width <= 0 || output_height <= 0 || logical_size.width <= 0 ||
            logical_size.height <= 0) {
            previous_time = std::chrono::steady_clock::now();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::min(
            now - previous_time,
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
            .width = static_cast<float>(output_width),
            .height = static_cast<float>(output_height),
            .mouse_scale_x =
                static_cast<float>(output_width) / static_cast<float>(logical_size.width),
            .mouse_scale_y =
                static_cast<float>(output_height) / static_cast<float>(logical_size.height),
            .player_radius_pixels = *player_radius * config.view.pixels_per_world_unit,
        };
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

        const auto alpha = std::clamp(static_cast<float>(step_result.remainder.count()) /
                                          static_cast<float>(accumulator.step_duration().count()),
                                      0.0F,
                                      1.0F);
        const auto camera = previous_player_position +
                            ((current_player_position - previous_player_position) * alpha);
        render_world(
            renderer.get(), output_width, output_height, camera, config, world, *player, food_ids);
    }
}

} // namespace dots::client
