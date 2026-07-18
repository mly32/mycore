#include "mycore/platform_sdl/window.hpp"

#include "mycore/platform_sdl/error.hpp"

#include <SDL3/SDL.h>
#include <utility>

namespace mycore::platform_sdl {
namespace {

SDL_WindowFlags to_sdl_flags(const WindowConfig& config) {
    SDL_WindowFlags flags{};
    if (has_flag(config.flags, WindowFlags::Resizable)) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (has_flag(config.flags, WindowFlags::Fullscreen)) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (has_flag(config.flags, WindowFlags::HighPixelDensity)) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (!config.visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    return flags;
}

} // namespace

Window::Window(const WindowConfig& config) {
    if (config.minimum_width < 0 || config.minimum_height < 0 ||
        (config.minimum_width > 0 && config.width < config.minimum_width) ||
        (config.minimum_height > 0 && config.height < config.minimum_height)) {
        throw StartupError{"Window size must satisfy its non-negative minimum size"};
    }
    window_ =
        SDL_CreateWindow(config.title.c_str(), config.width, config.height, to_sdl_flags(config));
    if (window_ == nullptr) {
        throw StartupError::from_sdl("Could not create SDL window");
    }
    if ((config.minimum_width > 0 || config.minimum_height > 0) &&
        !SDL_SetWindowMinimumSize(window_, config.minimum_width, config.minimum_height)) {
        const auto error = StartupError::from_sdl("Could not set SDL window minimum size");
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        throw error;
    }
}

Window::~Window() {
    SDL_DestroyWindow(window_);
}

Window::Window(Window&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        SDL_DestroyWindow(window_);
        window_ = std::exchange(other.window_, nullptr);
    }
    return *this;
}

void Window::show() {
    if (!SDL_ShowWindow(window_)) {
        throw StartupError::from_sdl("Could not show SDL window");
    }
}

void Window::hide() {
    if (!SDL_HideWindow(window_)) {
        throw StartupError::from_sdl("Could not hide SDL window");
    }
}

WindowSize Window::size() const {
    WindowSize result;
    if (!SDL_GetWindowSize(window_, &result.width, &result.height)) {
        throw StartupError::from_sdl("Could not query SDL window size");
    }
    return result;
}

WindowSize Window::pixel_size() const {
    WindowSize result;
    if (!SDL_GetWindowSizeInPixels(window_, &result.width, &result.height)) {
        throw StartupError::from_sdl("Could not query SDL window pixel size");
    }
    return result;
}

SDL_Window* Window::native_handle() const noexcept {
    return window_;
}

} // namespace mycore::platform_sdl
