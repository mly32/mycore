#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;

namespace mycore::platform_sdl {

enum class WindowFlags : std::uint8_t {
    None = 0,
    Resizable = 1U << 0U,
    Fullscreen = 1U << 1U,
    HighPixelDensity = 1U << 2U,
};

[[nodiscard]] constexpr WindowFlags operator|(WindowFlags lhs, WindowFlags rhs) noexcept {
    // bitfield enum creates composite values
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<WindowFlags>(static_cast<std::uint8_t>(lhs) |
                                    static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool has_flag(WindowFlags flags, WindowFlags flag) noexcept {
    return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(flag)) != 0U;
}

struct WindowConfig {
    std::string title{"MyCore"};
    int width{1280};
    int height{720};
    int minimum_width{};
    int minimum_height{};
    WindowFlags flags{WindowFlags::None};
    bool visible{true};
};

struct WindowSize {
    int width{};
    int height{};

    auto operator<=>(const WindowSize&) const = default;
};

class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    void show();
    void hide();
    [[nodiscard]] WindowSize size() const;
    [[nodiscard]] WindowSize pixel_size() const;
    [[nodiscard]] SDL_Window* native_handle() const noexcept;

private:
    SDL_Window* window_{};
};

} // namespace mycore::platform_sdl
