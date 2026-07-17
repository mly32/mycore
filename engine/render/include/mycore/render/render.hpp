#pragma once

#include "mycore/platform_sdl/window.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

struct SDL_GPUBuffer;
struct SDL_GPUCommandBuffer;
struct SDL_GPUDevice;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPURenderPass;
struct SDL_GPUShader;
struct SDL_GPUTexture;

namespace mycore::render {

class Error : public std::runtime_error {
public:
    explicit Error(std::string message);
    [[nodiscard]] static Error from_sdl(std::string_view operation);
};

enum class ShaderFormat {
    SpirV,
    Dxil,
    Msl,
};

enum class ShaderStage {
    Vertex,
    Fragment,
};

enum class PresentMode {
    Vsync,
    Mailbox,
    Immediate,
};

enum class RenderPassLoadOperation {
    Clear,
    Load,
};

enum class VertexFormat {
    Float,
    Float2,
    Float4,
};

enum class VertexInputRate {
    Vertex,
    Instance,
};

[[nodiscard]] constexpr ShaderFormat platform_shader_format() noexcept;

struct Color {
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};

    auto operator<=>(const Color&) const = default;
};

struct DeviceConfig {
    ShaderFormat shader_format{platform_shader_format()};
    bool debug_mode{};
    bool vsync{true};
};

struct BufferDescription {
    std::size_t size{};
    std::string_view label;
};

struct ShaderDescription {
    ShaderStage stage{};
    std::string_view entry_point{"main"};
    std::uint32_t sampler_count{};
    std::uint32_t storage_texture_count{};
    std::uint32_t storage_buffer_count{};
    std::uint32_t uniform_buffer_count{};
    std::string_view label;
};

struct VertexBufferLayout {
    std::uint32_t slot{};
    std::uint32_t stride{};
    VertexInputRate input_rate{VertexInputRate::Vertex};
};

struct VertexAttribute {
    std::uint32_t location{};
    std::uint32_t buffer_slot{};
    VertexFormat format{VertexFormat::Float};
    std::uint32_t offset{};
};

class Device;
class CommandList;
class RenderPass;

class Buffer {
public:
    Buffer() = default;
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend class Device;
    friend class CommandList;
    friend class RenderPass;

    Buffer(Device& device, SDL_GPUBuffer* buffer, std::size_t size) noexcept;
    void reset() noexcept;

    Device* device_{};
    SDL_GPUBuffer* buffer_{};
    std::size_t size_{};
};

class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend class Device;

    Shader(Device& device, SDL_GPUShader* shader) noexcept;
    void reset() noexcept;

    Device* device_{};
    SDL_GPUShader* shader_{};
};

struct GraphicsPipelineDescription {
    const Shader* vertex_shader{};
    const Shader* fragment_shader{};
    std::span<const VertexBufferLayout> vertex_buffers;
    std::span<const VertexAttribute> vertex_attributes;
    bool enable_blending{};
    std::string_view label;
};

class GraphicsPipeline {
public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend class Device;
    friend class RenderPass;

    GraphicsPipeline(Device& device, SDL_GPUGraphicsPipeline* pipeline) noexcept;
    void reset() noexcept;

    Device* device_{};
    SDL_GPUGraphicsPipeline* pipeline_{};
};

struct BufferBinding {
    const Buffer* buffer{};
    std::size_t offset{};
};

class SwapchainTarget {
public:
    SwapchainTarget() = default;
    SwapchainTarget(const SwapchainTarget&) = delete;
    SwapchainTarget& operator=(const SwapchainTarget&) = delete;
    SwapchainTarget(SwapchainTarget&& other) noexcept;
    SwapchainTarget& operator=(SwapchainTarget&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;

private:
    friend class CommandList;

    SwapchainTarget(CommandList& commands,
                    SDL_GPUTexture* texture,
                    std::uint32_t width,
                    std::uint32_t height) noexcept;

    CommandList* commands_{};
    SDL_GPUTexture* texture_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
};

class RenderPass {
public:
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    RenderPass(RenderPass&&) = delete;
    RenderPass& operator=(RenderPass&&) = delete;

    void bind_pipeline(const GraphicsPipeline& pipeline);
    void bind_vertex_buffers(std::uint32_t first_slot, std::span<const BufferBinding> bindings);
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1);
    void end() noexcept;
    [[nodiscard]] SDL_GPURenderPass* native_handle() const noexcept;

private:
    friend class CommandList;

    RenderPass(CommandList& commands, SDL_GPURenderPass* render_pass) noexcept;

    CommandList* commands_{};
    SDL_GPURenderPass* render_pass_{};
};

class CommandList {
public:
    ~CommandList();

    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;
    CommandList(CommandList&&) = delete;
    CommandList& operator=(CommandList&&) = delete;

    void upload(Buffer& destination,
                std::span<const std::byte> bytes,
                std::size_t offset = 0,
                bool cycle = true);
    [[nodiscard]] SwapchainTarget acquire_swapchain();
    [[nodiscard]] RenderPass
    begin_render_pass(const SwapchainTarget& target,
                      Color clear_color = {},
                      RenderPassLoadOperation load_operation = RenderPassLoadOperation::Clear);
    void push_vertex_uniform(std::uint32_t slot, std::span<const std::byte> bytes);
    void push_fragment_uniform(std::uint32_t slot, std::span<const std::byte> bytes);
    void submit();
    [[nodiscard]] SDL_GPUCommandBuffer* native_handle() const noexcept;

    template <typename Type> void push_vertex_uniform(std::uint32_t slot, const Type& value) {
        push_vertex_uniform(slot, std::as_bytes(std::span{&value, std::size_t{1}}));
    }

    template <typename Type> void push_fragment_uniform(std::uint32_t slot, const Type& value) {
        push_fragment_uniform(slot, std::as_bytes(std::span{&value, std::size_t{1}}));
    }

private:
    friend class Device;
    friend class RenderPass;

    CommandList(Device& device, SDL_GPUCommandBuffer* commands) noexcept;
    void finish_without_throwing() noexcept;

    Device* device_{};
    SDL_GPUCommandBuffer* commands_{};
    bool swapchain_acquired_{};
    bool render_pass_active_{};
};

class Device {
public:
    explicit Device(platform_sdl::Window& window, DeviceConfig config = {});
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] ShaderFormat shader_format() const noexcept;
    [[nodiscard]] PresentMode present_mode() const noexcept;
    [[nodiscard]] std::string_view driver_name() const noexcept;
    [[nodiscard]] SDL_GPUDevice* native_handle() const noexcept;
    [[nodiscard]] Buffer create_buffer(const BufferDescription& description);
    [[nodiscard]] Shader create_shader(std::span<const std::byte> code,
                                       const ShaderDescription& description);
    [[nodiscard]] GraphicsPipeline
    create_graphics_pipeline(const GraphicsPipelineDescription& description);
    [[nodiscard]] CommandList acquire_command_list();

private:
    friend class Buffer;
    friend class CommandList;
    friend class GraphicsPipeline;
    friend class Shader;

    SDL_GPUDevice* device_{};
    platform_sdl::Window* window_{};
    ShaderFormat shader_format_{};
    PresentMode present_mode_{PresentMode::Vsync};
};

[[nodiscard]] constexpr ShaderFormat platform_shader_format() noexcept {
#if defined(_WIN32)
    return ShaderFormat::Dxil;
#elif defined(__APPLE__)
    return ShaderFormat::Msl;
#else
    return ShaderFormat::SpirV;
#endif
}

[[nodiscard]] constexpr std::string_view shader_file_extension(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::SpirV:
        return "spv";
    case ShaderFormat::Dxil:
        return "dxil";
    case ShaderFormat::Msl:
        return "msl";
    }
    return {};
}

[[nodiscard]] constexpr std::string_view shader_entry_point(ShaderFormat format) noexcept {
    return format == ShaderFormat::Msl ? "main0" : "main";
}

[[nodiscard]] constexpr PresentMode
choose_present_mode(bool vsync, bool immediate_supported, bool mailbox_supported) noexcept {
    if (vsync) {
        return PresentMode::Vsync;
    }
    if (immediate_supported) {
        return PresentMode::Immediate;
    }
    return mailbox_supported ? PresentMode::Mailbox : PresentMode::Vsync;
}

} // namespace mycore::render
