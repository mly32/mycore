#include "mycore/debug_ui/context.hpp"

#include "mycore/platform_sdl/window.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <utility>

namespace mycore::debug_ui {

Error::Error(std::string message)
    : std::runtime_error(std::move(message)) {}

Context::Context(platform_sdl::Window& window, render::Device& device) {
    IMGUI_CHECKVERSION();
    if (ImGui::GetCurrentContext() != nullptr) {
        throw Error{"Dear ImGui already has an active context"};
    }
    context_ = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForSDLGPU(window.native_handle())) {
        ImGui::DestroyContext(context_);
        context_ = nullptr;
        throw Error{"Could not initialize the Dear ImGui SDL3 platform backend"};
    }
    platform_initialized_ = true;

    ImGui_ImplSDLGPU3_InitInfo init_info{
        .Device = device.native_handle(),
        .ColorTargetFormat =
            SDL_GetGPUSwapchainTextureFormat(device.native_handle(), window.native_handle()),
        .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
    };
    if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
        ImGui_ImplSDL3_Shutdown();
        platform_initialized_ = false;
        ImGui::DestroyContext(context_);
        context_ = nullptr;
        throw Error{"Could not initialize the Dear ImGui SDL_GPU renderer backend"};
    }
    renderer_initialized_ = true;
}

Context::~Context() {
    ImGui::SetCurrentContext(context_);
    cancel_frame();
    if (renderer_initialized_) {
        ImGui_ImplSDLGPU3_Shutdown();
    }
    if (platform_initialized_) {
        ImGui_ImplSDL3_Shutdown();
    }
    if (context_ != nullptr) {
        ImGui::DestroyContext(context_);
        context_ = nullptr;
    }
}

void Context::process_event(const SDL_Event& event) {
    ImGui::SetCurrentContext(context_);
    static_cast<void>(ImGui_ImplSDL3_ProcessEvent(&event));
}

void Context::begin_frame() {
    ImGui::SetCurrentContext(context_);
    if (frame_active_) {
        throw Error{"A Dear ImGui frame is already active"};
    }
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    frame_active_ = true;
}

void Context::render(render::CommandList& commands, const render::SwapchainTarget& target) {
    ImGui::SetCurrentContext(context_);
    if (!frame_active_) {
        throw Error{"Cannot render Dear ImGui without beginning a frame"};
    }
    ImGui::Render();
    frame_active_ = false;

    auto* draw_data = ImGui::GetDrawData();
    ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, commands.native_handle());
    auto pass = commands.begin_render_pass(target, {}, render::RenderPassLoadOperation::Load);
    ImGui_ImplSDLGPU3_RenderDrawData(draw_data, commands.native_handle(), pass.native_handle());
    pass.end();
}

void Context::cancel_frame() noexcept {
    ImGui::SetCurrentContext(context_);
    if (frame_active_) {
        ImGui::EndFrame();
        frame_active_ = false;
    }
}

} // namespace mycore::debug_ui
