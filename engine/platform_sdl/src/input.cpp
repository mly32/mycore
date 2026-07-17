#include "mycore/platform_sdl/input.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstddef>

namespace mycore::platform_sdl {
namespace {

constexpr std::array<SDL_Scancode, static_cast<std::size_t>(Key::Count)> kScancodes{
    SDL_SCANCODE_A,         SDL_SCANCODE_B,      SDL_SCANCODE_C,        SDL_SCANCODE_D,
    SDL_SCANCODE_E,         SDL_SCANCODE_F,      SDL_SCANCODE_G,        SDL_SCANCODE_H,
    SDL_SCANCODE_I,         SDL_SCANCODE_J,      SDL_SCANCODE_K,        SDL_SCANCODE_L,
    SDL_SCANCODE_M,         SDL_SCANCODE_N,      SDL_SCANCODE_O,        SDL_SCANCODE_P,
    SDL_SCANCODE_Q,         SDL_SCANCODE_R,      SDL_SCANCODE_S,        SDL_SCANCODE_T,
    SDL_SCANCODE_U,         SDL_SCANCODE_V,      SDL_SCANCODE_W,        SDL_SCANCODE_X,
    SDL_SCANCODE_Y,         SDL_SCANCODE_Z,      SDL_SCANCODE_0,        SDL_SCANCODE_1,
    SDL_SCANCODE_2,         SDL_SCANCODE_3,      SDL_SCANCODE_4,        SDL_SCANCODE_5,
    SDL_SCANCODE_6,         SDL_SCANCODE_7,      SDL_SCANCODE_8,        SDL_SCANCODE_9,
    SDL_SCANCODE_UP,        SDL_SCANCODE_DOWN,   SDL_SCANCODE_LEFT,     SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_ESCAPE,    SDL_SCANCODE_SPACE,  SDL_SCANCODE_RETURN,   SDL_SCANCODE_TAB,
    SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT,   SDL_SCANCODE_LCTRL,
    SDL_SCANCODE_RCTRL,     SDL_SCANCODE_LALT,   SDL_SCANCODE_RALT,     SDL_SCANCODE_LGUI,
    SDL_SCANCODE_RGUI,      SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE,   SDL_SCANCODE_HOME,
    SDL_SCANCODE_END,       SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_F1,
    SDL_SCANCODE_F2,        SDL_SCANCODE_F3,     SDL_SCANCODE_F4,       SDL_SCANCODE_F5,
    SDL_SCANCODE_F6,        SDL_SCANCODE_F7,     SDL_SCANCODE_F8,       SDL_SCANCODE_F9,
    SDL_SCANCODE_F10,       SDL_SCANCODE_F11,    SDL_SCANCODE_F12,
};

constexpr std::array<SDL_MouseButtonFlags, static_cast<std::size_t>(MouseButton::Count)>
    kMouseButtonMasks{
        SDL_BUTTON_LMASK,
        SDL_BUTTON_MMASK,
        SDL_BUTTON_RMASK,
        SDL_BUTTON_X1MASK,
        SDL_BUTTON_X2MASK,
    };

template <typename Enum, std::size_t Size>
[[nodiscard]] constexpr std::size_t enum_index(Enum value, const std::array<bool, Size>&) {
    return static_cast<std::size_t>(value);
}

} // namespace

KeyboardSnapshot::KeyboardSnapshot(std::initializer_list<Key> pressed_keys) {
    for (const auto key : pressed_keys) {
        set_pressed(key, true);
    }
}

bool KeyboardSnapshot::pressed(Key key) const noexcept {
    const auto index = enum_index(key, pressed_);
    return index < pressed_.size() && pressed_[index];
}

void KeyboardSnapshot::set_pressed(Key key, bool pressed_value) noexcept {
    const auto index = enum_index(key, pressed_);
    if (index < pressed_.size()) {
        pressed_[index] = pressed_value;
    }
}

MouseSnapshot::MouseSnapshot(float x, float y, std::initializer_list<MouseButton> pressed_buttons)
    : x_(x),
      y_(y) {
    for (const auto button : pressed_buttons) {
        set_pressed(button, true);
    }
}

float MouseSnapshot::x() const noexcept {
    return x_;
}

float MouseSnapshot::y() const noexcept {
    return y_;
}

bool MouseSnapshot::pressed(MouseButton button) const noexcept {
    const auto index = enum_index(button, pressed_);
    return index < pressed_.size() && pressed_[index];
}

void MouseSnapshot::set_pressed(MouseButton button, bool pressed_value) noexcept {
    const auto index = enum_index(button, pressed_);
    if (index < pressed_.size()) {
        pressed_[index] = pressed_value;
    }
}

InputSnapshot poll_input(Window& window) {
    InputSnapshot snapshot;
    SDL_Event event;
    const auto window_id = SDL_GetWindowID(window.native_handle());
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                             event.window.windowID == window_id)) {
            snapshot.quit_requested = true;
        }
    }

    int key_count{};
    const auto* keyboard_state = SDL_GetKeyboardState(&key_count);
    for (std::size_t index = 0; index < kScancodes.size(); ++index) {
        const auto scancode = kScancodes[index];
        const auto scancode_index = static_cast<int>(scancode);
        if (scancode_index >= 0 && scancode_index < key_count) {
            snapshot.keyboard.set_pressed(static_cast<Key>(index), keyboard_state[scancode_index]);
        }
    }

    float mouse_x{};
    float mouse_y{};
    const auto mouse_state = SDL_GetMouseState(&mouse_x, &mouse_y);
    snapshot.mouse.x_ = mouse_x;
    snapshot.mouse.y_ = mouse_y;
    for (std::size_t index = 0; index < kMouseButtonMasks.size(); ++index) {
        snapshot.mouse.set_pressed(static_cast<MouseButton>(index),
                                   (mouse_state & kMouseButtonMasks[index]) != 0U);
    }
    return snapshot;
}

} // namespace mycore::platform_sdl
