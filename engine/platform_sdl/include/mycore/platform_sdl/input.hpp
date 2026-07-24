#pragma once

#include "mycore/platform_sdl/window.hpp"

#include <array>
#include <cstddef>
#include <initializer_list>

union SDL_Event;

namespace mycore::platform_sdl {

struct InputSnapshot;

class EventObserver {
public:
    virtual ~EventObserver() = default;
    virtual void process_event(const SDL_Event& event) = 0;
};

enum class Key : std::uint8_t {
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Up,
    Down,
    Left,
    Right,
    Escape,
    Space,
    Enter,
    Tab,
    Backspace,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftGui,
    RightGui,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Count,
};

enum class MouseButton : std::uint8_t {
    Left,
    Middle,
    Right,
    X1,
    X2,
    Count,
};

class KeyboardSnapshot {
public:
    KeyboardSnapshot() = default;
    KeyboardSnapshot(std::initializer_list<Key> pressed_keys);

    [[nodiscard]] bool pressed(Key key) const noexcept;

private:
    friend InputSnapshot poll_input(Window& window, EventObserver* observer);
    void set_pressed(Key key, bool pressed) noexcept;

    std::array<bool, static_cast<std::size_t>(Key::Count)> pressed_{};
};

class MouseSnapshot {
public:
    MouseSnapshot() = default;
    MouseSnapshot(float x, float y, std::initializer_list<MouseButton> pressed_buttons = {});

    [[nodiscard]] float x() const noexcept;
    [[nodiscard]] float y() const noexcept;
    [[nodiscard]] bool pressed(MouseButton button) const noexcept;

private:
    friend InputSnapshot poll_input(Window& window, EventObserver* observer);
    void set_pressed(MouseButton button, bool pressed) noexcept;

    float x_{};
    float y_{};
    std::array<bool, static_cast<std::size_t>(MouseButton::Count)> pressed_{};
};

struct InputSnapshot {
    KeyboardSnapshot keyboard;
    MouseSnapshot mouse;
    float wheel_delta_y{};
    bool quit_requested{};
};

[[nodiscard]] InputSnapshot poll_input(Window& window, EventObserver* observer = nullptr);

} // namespace mycore::platform_sdl
