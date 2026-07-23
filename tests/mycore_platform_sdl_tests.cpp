#include "mycore/platform_sdl/error.hpp"
#include "mycore/platform_sdl/input.hpp"
#include "mycore/platform_sdl/runtime.hpp"
#include "mycore/platform_sdl/window.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <type_traits>

namespace {

class RecordingEventObserver final : public mycore::platform_sdl::EventObserver {
public:
    void process_event(const SDL_Event& event) override {
        saw_user_event = saw_user_event || event.type == SDL_EVENT_USER;
    }

    bool saw_user_event{};
};

} // namespace

TEST_CASE("Platform input snapshots default to released and idle", "[platform][input]") {
    const mycore::platform_sdl::InputSnapshot snapshot;

    REQUIRE_FALSE(snapshot.keyboard.pressed(mycore::platform_sdl::Key::W));
    REQUIRE_FALSE(snapshot.keyboard.pressed(mycore::platform_sdl::Key::Count));
    REQUIRE(snapshot.mouse.x() == 0.0F);
    REQUIRE(snapshot.mouse.y() == 0.0F);
    REQUIRE_FALSE(snapshot.mouse.pressed(mycore::platform_sdl::MouseButton::Left));
    REQUIRE_FALSE(snapshot.mouse.pressed(mycore::platform_sdl::MouseButton::Count));
    REQUIRE(snapshot.wheel_delta_y == 0.0F);
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

TEST_CASE("Platform input snapshots accumulate and consume vertical wheel events",
          "[platform][input]") {
    mycore::platform_sdl::Runtime runtime;
    mycore::platform_sdl::Window window{{.visible = false}};
    SDL_Event first{};
    first.type = SDL_EVENT_MOUSE_WHEEL;
    first.wheel.windowID = SDL_GetWindowID(window.native_handle());
    first.wheel.y = 1.5F;
    SDL_Event second = first;
    second.wheel.y = -0.25F;

    REQUIRE(SDL_PushEvent(&first));
    REQUIRE(SDL_PushEvent(&second));
    CHECK(mycore::platform_sdl::poll_input(window).wheel_delta_y == 1.25F);
    CHECK(mycore::platform_sdl::poll_input(window).wheel_delta_y == 0.0F);
}

TEST_CASE("Platform event observers receive events drained for an input snapshot",
          "[platform][input]") {
    mycore::platform_sdl::Runtime runtime;
    mycore::platform_sdl::Window window{{.visible = false}};
    RecordingEventObserver observer;
    SDL_Event event{};
    event.type = SDL_EVENT_USER;

    REQUIRE(SDL_PushEvent(&event));
    const auto snapshot = mycore::platform_sdl::poll_input(window, &observer);

    CHECK(observer.saw_user_event);
    CHECK_FALSE(snapshot.quit_requested);
}

TEST_CASE("Platform windows enforce configured logical minimum dimensions", "[platform][window]") {
    mycore::platform_sdl::Runtime runtime;
    mycore::platform_sdl::Window window{{
        .width = 500,
        .height = 500,
        .minimum_width = 500,
        .minimum_height = 500,
        .flags = mycore::platform_sdl::WindowFlags::Resizable,
        .visible = false,
    }};

    int minimum_width{};
    int minimum_height{};
    REQUIRE(SDL_GetWindowMinimumSize(window.native_handle(), &minimum_width, &minimum_height));
    CHECK(minimum_width == 500);
    CHECK(minimum_height == 500);

    CHECK_THROWS_AS(mycore::platform_sdl::Window({
                        .width = 499,
                        .height = 500,
                        .minimum_width = 500,
                        .minimum_height = 500,
                        .visible = false,
                    }),
                    mycore::platform_sdl::StartupError);
}

static_assert(!std::is_copy_constructible_v<mycore::platform_sdl::Window>);
static_assert(std::is_move_constructible_v<mycore::platform_sdl::Window>);
static_assert(!std::is_copy_constructible_v<mycore::platform_sdl::Runtime>);
static_assert(!std::is_move_constructible_v<mycore::platform_sdl::Runtime>);
