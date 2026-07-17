#include "dots/client/client_config.hpp"
#include "dots/client/controls.hpp"
#include "mycore/platform_sdl/input.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

constexpr dots::simulation::EntityId kPlayer{7};
constexpr dots::simulation::InputCommandId kCommand{12};
constexpr dots::client::InputViewport kViewport{
    .width = 200.0F,
    .height = 100.0F,
    .player_radius_pixels = 20.0F,
};

dots::simulation::InputCommand command_for(const mycore::platform_sdl::InputSnapshot& input,
                                           dots::client::ClientControls controls) {
    return dots::client::make_input_command(input, controls, kPlayer, kCommand, kViewport);
}

} // namespace

TEST_CASE("Keyboard movement normalizes diagonals and cancels opposing keys",
          "[dots][client][input]") {
    auto controls = dots::client::default_client_config().controls;
    controls.mode = dots::client::InputMode::Keyboard;

    const auto diagonal = command_for(
        {.keyboard = {mycore::platform_sdl::Key::W, mycore::platform_sdl::Key::D}}, controls);
    REQUIRE(diagonal.movement.x == Catch::Approx(0.7071067F));
    REQUIRE(diagonal.movement.y == Catch::Approx(-0.7071067F));

    const auto cancelled = command_for({.keyboard = {mycore::platform_sdl::Key::W,
                                                     mycore::platform_sdl::Key::S,
                                                     mycore::platform_sdl::Key::A,
                                                     mycore::platform_sdl::Key::D}},
                                       controls);
    REQUIRE(cancelled.movement == mycore::math::Vector2{});
}

TEST_CASE("Mouse movement uses renderer coordinates and honors its dead zone",
          "[dots][client][input]") {
    auto controls = dots::client::default_client_config().controls;
    controls.mode = dots::client::InputMode::Mouse;
    controls.mouse_dead_zone_pixels = 12.0F;

    const auto outside = command_for({.mouse = {130.0F, 90.0F}}, controls);
    REQUIRE(outside.movement.x == Catch::Approx(0.6F));
    REQUIRE(outside.movement.y == Catch::Approx(0.8F));

    const auto inside = command_for({.mouse = {107.0F, 58.0F}}, controls);
    REQUIRE(inside.movement == mycore::math::Vector2{});

    const auto inside_player_circle = command_for({.mouse = {118.0F, 50.0F}}, controls);
    REQUIRE(inside_player_circle.movement == mycore::math::Vector2{});

    const auto just_outside_player_circle = command_for({.mouse = {121.0F, 50.0F}}, controls);
    REQUIRE(just_outside_player_circle.movement == mycore::math::Vector2{1.0F, 0.0F});

    const auto scaled = dots::client::make_input_command(
        {.mouse = {65.0F, 45.0F}},
        controls,
        kPlayer,
        kCommand,
        {.width = 200.0F, .height = 100.0F, .mouse_scale_x = 2.0F, .mouse_scale_y = 2.0F});
    REQUIRE(scaled.movement.x == Catch::Approx(0.6F));
    REQUIRE(scaled.movement.y == Catch::Approx(0.8F));
}

TEST_CASE("Input modes have HUD labels", "[dots][client][input]") {
    REQUIRE(dots::client::input_mode_name(dots::client::InputMode::Mouse) == "MOUSE");
    REQUIRE(dots::client::input_mode_name(dots::client::InputMode::Keyboard) == "KEYBOARD");
    REQUIRE(dots::client::input_mode_name(dots::client::InputMode::Hybrid) == "HYBRID");
}

TEST_CASE("Mouse keyboard and hybrid modes select the expected movement", "[dots][client][input]") {
    const mycore::platform_sdl::InputSnapshot input{
        .keyboard = {mycore::platform_sdl::Key::A},
        .mouse = {200.0F, 50.0F},
    };
    auto controls = dots::client::default_client_config().controls;

    controls.mode = dots::client::InputMode::Mouse;
    REQUIRE(command_for(input, controls).movement == mycore::math::Vector2{1.0F, 0.0F});

    controls.mode = dots::client::InputMode::Keyboard;
    REQUIRE(command_for(input, controls).movement == mycore::math::Vector2{-1.0F, 0.0F});

    controls.mode = dots::client::InputMode::Hybrid;
    REQUIRE(command_for(input, controls).movement == mycore::math::Vector2{-1.0F, 0.0F});

    const mycore::platform_sdl::InputSnapshot mouse_only{.mouse = {200.0F, 50.0F}};
    REQUIRE(command_for(mouse_only, controls).movement == mycore::math::Vector2{1.0F, 0.0F});
}

TEST_CASE("Client input propagates IDs and handles configured quit bindings",
          "[dots][client][input]") {
    const auto controls = dots::client::default_client_config().controls;
    const mycore::platform_sdl::InputSnapshot input{
        .keyboard = {mycore::platform_sdl::Key::Escape},
    };

    const auto command = command_for(input, controls);
    REQUIRE(command.entity_id == kPlayer);
    REQUIRE(command.id == kCommand);
    REQUIRE(dots::client::quit_requested(input, controls));
    REQUIRE(dots::client::quit_requested({.quit_requested = true}, controls));
}
