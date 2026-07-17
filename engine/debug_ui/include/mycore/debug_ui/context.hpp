#pragma once

#include "mycore/platform_sdl/input.hpp"
#include "mycore/render/render.hpp"

#include <stdexcept>
#include <string>

struct ImGuiContext;

namespace mycore::platform_sdl {
class Window;
}

namespace mycore::debug_ui {

class Error : public std::runtime_error {
public:
    explicit Error(std::string message);
};

class Context final : public platform_sdl::EventObserver {
public:
    Context(platform_sdl::Window& window, render::Device& device);
    ~Context() override;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    void process_event(const SDL_Event& event) override;
    void begin_frame();
    void render(render::CommandList& commands, const render::SwapchainTarget& target);
    void cancel_frame() noexcept;

private:
    ImGuiContext* context_{};
    bool platform_initialized_{};
    bool renderer_initialized_{};
    bool frame_active_{};
};

} // namespace mycore::debug_ui
