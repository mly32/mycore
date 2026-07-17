#include "mycore/platform_sdl/input.hpp"
#include "mycore/platform_sdl/runtime.hpp"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

TEST_CASE("Platform input snapshots default to released and idle", "[platform][input]") {
    const mycore::platform_sdl::InputSnapshot snapshot;

    REQUIRE_FALSE(snapshot.keyboard.pressed(mycore::platform_sdl::Key::W));
    REQUIRE_FALSE(snapshot.keyboard.pressed(mycore::platform_sdl::Key::Count));
    REQUIRE(snapshot.mouse.x() == 0.0F);
    REQUIRE(snapshot.mouse.y() == 0.0F);
    REQUIRE_FALSE(snapshot.mouse.pressed(mycore::platform_sdl::MouseButton::Left));
    REQUIRE_FALSE(snapshot.mouse.pressed(mycore::platform_sdl::MouseButton::Count));
    REQUIRE_FALSE(snapshot.quit_requested);
}

TEST_CASE("Platform snapshots retain pressed keys and mouse state", "[platform][input]") {
    const mycore::platform_sdl::KeyboardSnapshot keyboard{
        mycore::platform_sdl::Key::A,
        mycore::platform_sdl::Key::Escape,
    };
    const mycore::platform_sdl::MouseSnapshot mouse{
        23.5F,
        41.0F,
        {mycore::platform_sdl::MouseButton::Left, mycore::platform_sdl::MouseButton::X2},
    };

    REQUIRE(keyboard.pressed(mycore::platform_sdl::Key::A));
    REQUIRE(keyboard.pressed(mycore::platform_sdl::Key::Escape));
    REQUIRE_FALSE(keyboard.pressed(mycore::platform_sdl::Key::D));
    REQUIRE(mouse.x() == 23.5F);
    REQUIRE(mouse.y() == 41.0F);
    REQUIRE(mouse.pressed(mycore::platform_sdl::MouseButton::Left));
    REQUIRE(mouse.pressed(mycore::platform_sdl::MouseButton::X2));
    REQUIRE_FALSE(mouse.pressed(mycore::platform_sdl::MouseButton::Right));
}

TEST_CASE("Platform input snapshots report quit state", "[platform][input]") {
    const mycore::platform_sdl::InputSnapshot snapshot{.quit_requested = true};
    REQUIRE(snapshot.quit_requested);
}

static_assert(!std::is_copy_constructible_v<mycore::platform_sdl::Window>);
static_assert(std::is_move_constructible_v<mycore::platform_sdl::Window>);
static_assert(!std::is_copy_constructible_v<mycore::platform_sdl::Runtime>);
static_assert(!std::is_move_constructible_v<mycore::platform_sdl::Runtime>);
