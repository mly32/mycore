#include "mycore/render/render.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace mycore::render {
namespace {

[[nodiscard]] SDL_GPUShaderFormat to_sdl(ShaderFormat format) {
    switch (format) {
    case ShaderFormat::SpirV:
        return SDL_GPU_SHADERFORMAT_SPIRV;
    case ShaderFormat::Dxil:
        return SDL_GPU_SHADERFORMAT_DXIL;
    case ShaderFormat::Msl:
        return SDL_GPU_SHADERFORMAT_MSL;
    }
    return SDL_GPU_SHADERFORMAT_INVALID;
}

[[nodiscard]] SDL_GPUShaderStage to_sdl(ShaderStage stage) {
    return stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
}

[[nodiscard]] SDL_GPUVertexElementFormat to_sdl(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float:
        return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    case VertexFormat::Float2:
        return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case VertexFormat::Float4:
        return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
}

[[nodiscard]] SDL_GPUVertexInputRate to_sdl(VertexInputRate rate) {
    return rate == VertexInputRate::Vertex ? SDL_GPU_VERTEXINPUTRATE_VERTEX
                                           : SDL_GPU_VERTEXINPUTRATE_INSTANCE;
}

[[nodiscard]] SDL_GPUPresentMode to_sdl(PresentMode mode) {
    switch (mode) {
    case PresentMode::Vsync:
        return SDL_GPU_PRESENTMODE_VSYNC;
    case PresentMode::Mailbox:
        return SDL_GPU_PRESENTMODE_MAILBOX;
    case PresentMode::Immediate:
        return SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}

[[nodiscard]] std::uint32_t checked_u32(std::size_t value, std::string_view field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw Error{std::string{field} + " exceeds the SDL_GPU 32-bit limit"};
    }
    return static_cast<std::uint32_t>(value);
}

void require_same_device(const Device* expected, const Device* actual, std::string_view resource) {
    if (actual == nullptr || expected != actual) {
        throw Error{std::string{resource} + " belongs to a different render device"};
    }
}

} // namespace

Error::Error(std::string message)
    : std::runtime_error(std::move(message)) {}

Error Error::from_sdl(std::string_view operation) {
    return Error{std::string{operation} + ": " + SDL_GetError()};
}

Buffer::Buffer(Device& device, SDL_GPUBuffer* buffer, std::size_t size) noexcept
    : device_(&device),
      buffer_(buffer),
      size_(size) {}

Buffer::~Buffer() {
    reset();
}

Buffer::Buffer(Buffer&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      buffer_(std::exchange(other.buffer_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        reset();
        device_ = std::exchange(other.device_, nullptr);
        buffer_ = std::exchange(other.buffer_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

std::size_t Buffer::size() const noexcept {
    return size_;
}

Buffer::operator bool() const noexcept {
    return buffer_ != nullptr;
}

void Buffer::reset() noexcept {
    if (buffer_ != nullptr) {
        SDL_ReleaseGPUBuffer(device_->device_, buffer_);
    }
    device_ = nullptr;
    buffer_ = nullptr;
    size_ = 0;
}

Shader::Shader(Device& device, SDL_GPUShader* shader) noexcept
    : device_(&device),
      shader_(shader) {}

Shader::~Shader() {
    reset();
}

Shader::Shader(Shader&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      shader_(std::exchange(other.shader_, nullptr)) {}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        reset();
        device_ = std::exchange(other.device_, nullptr);
        shader_ = std::exchange(other.shader_, nullptr);
    }
    return *this;
}

Shader::operator bool() const noexcept {
    return shader_ != nullptr;
}

void Shader::reset() noexcept {
    if (shader_ != nullptr) {
        SDL_ReleaseGPUShader(device_->device_, shader_);
    }
    device_ = nullptr;
    shader_ = nullptr;
}

GraphicsPipeline::GraphicsPipeline(Device& device, SDL_GPUGraphicsPipeline* pipeline) noexcept
    : device_(&device),
      pipeline_(pipeline) {}

GraphicsPipeline::~GraphicsPipeline() {
    reset();
}

GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      pipeline_(std::exchange(other.pipeline_, nullptr)) {}

GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept {
    if (this != &other) {
        reset();
        device_ = std::exchange(other.device_, nullptr);
        pipeline_ = std::exchange(other.pipeline_, nullptr);
    }
    return *this;
}

GraphicsPipeline::operator bool() const noexcept {
    return pipeline_ != nullptr;
}

void GraphicsPipeline::reset() noexcept {
    if (pipeline_ != nullptr) {
        SDL_ReleaseGPUGraphicsPipeline(device_->device_, pipeline_);
    }
    device_ = nullptr;
    pipeline_ = nullptr;
}

SwapchainTarget::SwapchainTarget(CommandList& commands,
                                 SDL_GPUTexture* texture,
                                 std::uint32_t width,
                                 std::uint32_t height) noexcept
    : commands_(&commands),
      texture_(texture),
      width_(width),
      height_(height) {}

SwapchainTarget::SwapchainTarget(SwapchainTarget&& other) noexcept
    : commands_(std::exchange(other.commands_, nullptr)),
      texture_(std::exchange(other.texture_, nullptr)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)) {}

SwapchainTarget& SwapchainTarget::operator=(SwapchainTarget&& other) noexcept {
    if (this != &other) {
        commands_ = std::exchange(other.commands_, nullptr);
        texture_ = std::exchange(other.texture_, nullptr);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
    }
    return *this;
}

SwapchainTarget::operator bool() const noexcept {
    return texture_ != nullptr && width_ > 0 && height_ > 0;
}

std::uint32_t SwapchainTarget::width() const noexcept {
    return width_;
}

std::uint32_t SwapchainTarget::height() const noexcept {
    return height_;
}

RenderPass::RenderPass(CommandList& commands, SDL_GPURenderPass* render_pass) noexcept
    : commands_(&commands),
      render_pass_(render_pass) {}

RenderPass::~RenderPass() {
    end();
}

void RenderPass::bind_pipeline(const GraphicsPipeline& pipeline) {
    if (render_pass_ == nullptr) {
        throw Error{"Cannot bind a pipeline after the render pass has ended"};
    }
    require_same_device(commands_->device_, pipeline.device_, "Graphics pipeline");
    SDL_BindGPUGraphicsPipeline(render_pass_, pipeline.pipeline_);
}

void RenderPass::bind_vertex_buffers(std::uint32_t first_slot,
                                     std::span<const BufferBinding> bindings) {
    if (render_pass_ == nullptr) {
        throw Error{"Cannot bind buffers after the render pass has ended"};
    }
    std::vector<SDL_GPUBufferBinding> native_bindings;
    native_bindings.reserve(bindings.size());
    for (const auto& binding : bindings) {
        if (binding.buffer == nullptr || !*binding.buffer) {
            throw Error{"Cannot bind an empty vertex buffer"};
        }
        require_same_device(commands_->device_, binding.buffer->device_, "Vertex buffer");
        if (binding.offset > binding.buffer->size_) {
            throw Error{"Vertex buffer binding offset exceeds its size"};
        }
        native_bindings.push_back({
            .buffer = binding.buffer->buffer_,
            .offset = checked_u32(binding.offset, "Vertex buffer binding offset"),
        });
    }
    SDL_BindGPUVertexBuffers(render_pass_,
                             first_slot,
                             native_bindings.data(),
                             checked_u32(native_bindings.size(), "Vertex buffer binding count"));
}

void RenderPass::draw(std::uint32_t vertex_count, std::uint32_t instance_count) {
    if (render_pass_ == nullptr) {
        throw Error{"Cannot draw after the render pass has ended"};
    }
    if (vertex_count == 0 || instance_count == 0) {
        return;
    }
    SDL_DrawGPUPrimitives(render_pass_, vertex_count, instance_count, 0, 0);
}

void RenderPass::end() noexcept {
    if (render_pass_ != nullptr) {
        SDL_EndGPURenderPass(render_pass_);
        render_pass_ = nullptr;
        commands_->render_pass_active_ = false;
    }
}

CommandList::CommandList(Device& device, SDL_GPUCommandBuffer* commands) noexcept
    : device_(&device),
      commands_(commands) {}

CommandList::~CommandList() {
    finish_without_throwing();
}

void CommandList::upload(Buffer& destination,
                         std::span<const std::byte> bytes,
                         std::size_t offset,
                         bool cycle) {
    if (commands_ == nullptr) {
        throw Error{"Cannot upload through a submitted command list"};
    }
    if (render_pass_active_) {
        throw Error{"GPU uploads must be recorded outside a render pass"};
    }
    require_same_device(device_, destination.device_, "Upload buffer");
    if (offset > destination.size_ || bytes.size() > destination.size_ - offset) {
        throw Error{"GPU buffer upload exceeds the destination buffer"};
    }
    if (bytes.empty()) {
        return;
    }

    const auto byte_count = checked_u32(bytes.size(), "GPU upload size");
    const SDL_GPUTransferBufferCreateInfo transfer_description{
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = byte_count,
        .props = 0,
    };
    auto* transfer = SDL_CreateGPUTransferBuffer(device_->device_, &transfer_description);
    if (transfer == nullptr) {
        throw Error::from_sdl("Could not create GPU transfer buffer");
    }
    auto* mapped = SDL_MapGPUTransferBuffer(device_->device_, transfer, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_->device_, transfer);
        throw Error::from_sdl("Could not map GPU transfer buffer");
    }
    std::memcpy(mapped, bytes.data(), bytes.size());
    SDL_UnmapGPUTransferBuffer(device_->device_, transfer);

    auto* copy_pass = SDL_BeginGPUCopyPass(commands_);
    if (copy_pass == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_->device_, transfer);
        throw Error::from_sdl("Could not begin GPU copy pass");
    }
    const SDL_GPUTransferBufferLocation source{
        .transfer_buffer = transfer,
        .offset = 0,
    };
    const SDL_GPUBufferRegion destination_region{
        .buffer = destination.buffer_,
        .offset = checked_u32(offset, "GPU upload offset"),
        .size = byte_count,
    };
    SDL_UploadToGPUBuffer(copy_pass, &source, &destination_region, cycle);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_ReleaseGPUTransferBuffer(device_->device_, transfer);
}

SwapchainTarget CommandList::acquire_swapchain() {
    if (commands_ == nullptr) {
        throw Error{"Cannot acquire a swapchain through a submitted command list"};
    }
    if (swapchain_acquired_) {
        throw Error{"A swapchain texture has already been acquired for this command list"};
    }

    SDL_GPUTexture* texture{};
    std::uint32_t width{};
    std::uint32_t height{};
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            commands_, device_->window_->native_handle(), &texture, &width, &height)) {
        throw Error::from_sdl("Could not acquire GPU swapchain texture");
    }
    swapchain_acquired_ = true;
    return SwapchainTarget{*this, texture, width, height};
}

RenderPass CommandList::begin_render_pass(const SwapchainTarget& target, Color clear_color) {
    if (commands_ == nullptr || target.commands_ != this || target.texture_ == nullptr) {
        throw Error{"Cannot begin a render pass with an invalid swapchain target"};
    }
    if (render_pass_active_) {
        throw Error{"A render pass is already active on this command list"};
    }
    const SDL_GPUColorTargetInfo target_info{
        .texture = target.texture_,
        .mip_level = 0,
        .layer_or_depth_plane = 0,
        .clear_color = {clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha},
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
        .resolve_texture = nullptr,
        .resolve_mip_level = 0,
        .resolve_layer = 0,
        .cycle = false,
        .cycle_resolve_texture = false,
        .padding1 = 0,
        .padding2 = 0,
    };
    auto* render_pass = SDL_BeginGPURenderPass(commands_, &target_info, 1, nullptr);
    if (render_pass == nullptr) {
        throw Error::from_sdl("Could not begin GPU render pass");
    }
    render_pass_active_ = true;
    return RenderPass{*this, render_pass};
}

void CommandList::push_vertex_uniform(std::uint32_t slot, std::span<const std::byte> bytes) {
    if (commands_ == nullptr || bytes.empty()) {
        throw Error{"Vertex uniform data must be pushed through an active command list"};
    }
    SDL_PushGPUVertexUniformData(
        commands_, slot, bytes.data(), checked_u32(bytes.size(), "Vertex uniform size"));
}

void CommandList::push_fragment_uniform(std::uint32_t slot, std::span<const std::byte> bytes) {
    if (commands_ == nullptr || bytes.empty()) {
        throw Error{"Fragment uniform data must be pushed through an active command list"};
    }
    SDL_PushGPUFragmentUniformData(
        commands_, slot, bytes.data(), checked_u32(bytes.size(), "Fragment uniform size"));
}

void CommandList::submit() {
    if (commands_ == nullptr) {
        throw Error{"Command list has already been submitted"};
    }
    if (render_pass_active_) {
        throw Error{"Cannot submit while a render pass is active"};
    }
    auto* commands = std::exchange(commands_, nullptr);
    if (!SDL_SubmitGPUCommandBuffer(commands)) {
        throw Error::from_sdl("Could not submit GPU command list");
    }
}

void CommandList::finish_without_throwing() noexcept {
    if (commands_ == nullptr) {
        return;
    }
    if (render_pass_active_) {
        render_pass_active_ = false;
    }
    auto* commands = std::exchange(commands_, nullptr);
    if (swapchain_acquired_) {
        static_cast<void>(SDL_SubmitGPUCommandBuffer(commands));
    } else {
        static_cast<void>(SDL_CancelGPUCommandBuffer(commands));
    }
}

Device::Device(platform_sdl::Window& window, DeviceConfig config)
    : window_(&window),
      shader_format_(config.shader_format) {
    device_ = SDL_CreateGPUDevice(to_sdl(shader_format_), config.debug_mode, nullptr);
    if (device_ == nullptr) {
        throw Error::from_sdl("Could not create SDL GPU device");
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window.native_handle())) {
        auto error = Error::from_sdl("Could not claim SDL window for the GPU device");
        SDL_DestroyGPUDevice(std::exchange(device_, nullptr));
        throw error;
    }

    const auto immediate_supported = SDL_WindowSupportsGPUPresentMode(
        device_, window.native_handle(), SDL_GPU_PRESENTMODE_IMMEDIATE);
    const auto mailbox_supported = SDL_WindowSupportsGPUPresentMode(
        device_, window.native_handle(), SDL_GPU_PRESENTMODE_MAILBOX);
    present_mode_ = choose_present_mode(config.vsync, immediate_supported, mailbox_supported);
    if (!SDL_SetGPUSwapchainParameters(device_,
                                       window.native_handle(),
                                       SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       to_sdl(present_mode_))) {
        auto error = Error::from_sdl("Could not configure the GPU swapchain");
        SDL_ReleaseWindowFromGPUDevice(device_, window.native_handle());
        SDL_DestroyGPUDevice(std::exchange(device_, nullptr));
        throw error;
    }
}

Device::~Device() {
    if (device_ != nullptr) {
        static_cast<void>(SDL_WaitForGPUIdle(device_));
        SDL_ReleaseWindowFromGPUDevice(device_, window_->native_handle());
        SDL_DestroyGPUDevice(device_);
    }
}

ShaderFormat Device::shader_format() const noexcept {
    return shader_format_;
}

PresentMode Device::present_mode() const noexcept {
    return present_mode_;
}

std::string_view Device::driver_name() const noexcept {
    const auto* name = SDL_GetGPUDeviceDriver(device_);
    return name == nullptr ? std::string_view{} : std::string_view{name};
}

Buffer Device::create_buffer(const BufferDescription& description) {
    if (description.size == 0) {
        throw Error{"GPU buffer size must be positive"};
    }
    const SDL_GPUBufferCreateInfo native_description{
        .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
        .size = checked_u32(description.size, "GPU buffer size"),
        .props = 0,
    };
    auto* buffer = SDL_CreateGPUBuffer(device_, &native_description);
    if (buffer == nullptr) {
        throw Error::from_sdl("Could not create GPU buffer");
    }
    if (!description.label.empty()) {
        const std::string label{description.label};
        SDL_SetGPUBufferName(device_, buffer, label.c_str());
    }
    return Buffer{*this, buffer, description.size};
}

Shader Device::create_shader(std::span<const std::byte> code,
                             const ShaderDescription& description) {
    if (code.empty()) {
        throw Error{"GPU shader code must not be empty"};
    }
    if (description.entry_point.empty()) {
        throw Error{"GPU shader entry point must not be empty"};
    }
    const std::string entry_point{description.entry_point};
    const SDL_GPUShaderCreateInfo native_description{
        .code_size = code.size(),
        .code = reinterpret_cast<const std::uint8_t*>(code.data()),
        .entrypoint = entry_point.c_str(),
        .format = to_sdl(shader_format_),
        .stage = to_sdl(description.stage),
        .num_samplers = description.sampler_count,
        .num_storage_textures = description.storage_texture_count,
        .num_storage_buffers = description.storage_buffer_count,
        .num_uniform_buffers = description.uniform_buffer_count,
        .props = 0,
    };
    auto* shader = SDL_CreateGPUShader(device_, &native_description);
    if (shader == nullptr) {
        throw Error::from_sdl("Could not create GPU shader");
    }
    return Shader{*this, shader};
}

GraphicsPipeline Device::create_graphics_pipeline(const GraphicsPipelineDescription& description) {
    if (description.vertex_shader == nullptr || description.fragment_shader == nullptr ||
        !*description.vertex_shader || !*description.fragment_shader) {
        throw Error{"Graphics pipeline requires vertex and fragment shaders"};
    }
    require_same_device(this, description.vertex_shader->device_, "Vertex shader");
    require_same_device(this, description.fragment_shader->device_, "Fragment shader");

    std::vector<SDL_GPUVertexBufferDescription> buffer_layouts;
    buffer_layouts.reserve(description.vertex_buffers.size());
    for (const auto& layout : description.vertex_buffers) {
        if (layout.stride == 0) {
            throw Error{"Graphics pipeline vertex-buffer stride must be positive"};
        }
        buffer_layouts.push_back({
            .slot = layout.slot,
            .pitch = layout.stride,
            .input_rate = to_sdl(layout.input_rate),
            .instance_step_rate = 0,
        });
    }

    std::vector<SDL_GPUVertexAttribute> attributes;
    attributes.reserve(description.vertex_attributes.size());
    for (const auto& attribute : description.vertex_attributes) {
        attributes.push_back({
            .location = attribute.location,
            .buffer_slot = attribute.buffer_slot,
            .format = to_sdl(attribute.format),
            .offset = attribute.offset,
        });
    }

    SDL_GPUColorTargetDescription color_target{
        .format = SDL_GetGPUSwapchainTextureFormat(device_, window_->native_handle()),
        .blend_state = {},
    };
    if (description.enable_blending) {
        color_target.blend_state = {
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .color_blend_op = SDL_GPU_BLENDOP_ADD,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
            .color_write_mask = 0,
            .enable_blend = true,
            .enable_color_write_mask = false,
            .padding1 = 0,
            .padding2 = 0,
        };
    }

    const SDL_GPUGraphicsPipelineCreateInfo native_description{
        .vertex_shader = description.vertex_shader->shader_,
        .fragment_shader = description.fragment_shader->shader_,
        .vertex_input_state =
            {
                .vertex_buffer_descriptions = buffer_layouts.data(),
                .num_vertex_buffers =
                    checked_u32(buffer_layouts.size(), "Vertex buffer layout count"),
                .vertex_attributes = attributes.data(),
                .num_vertex_attributes = checked_u32(attributes.size(), "Vertex attribute count"),
            },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {},
        .multisample_state = {},
        .depth_stencil_state = {},
        .target_info =
            {
                .color_target_descriptions = &color_target,
                .num_color_targets = 1,
                .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID,
                .has_depth_stencil_target = false,
                .padding1 = 0,
                .padding2 = 0,
                .padding3 = 0,
            },
        .props = 0,
    };
    auto* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &native_description);
    if (pipeline == nullptr) {
        throw Error::from_sdl("Could not create GPU graphics pipeline");
    }
    return GraphicsPipeline{*this, pipeline};
}

CommandList Device::acquire_command_list() {
    auto* commands = SDL_AcquireGPUCommandBuffer(device_);
    if (commands == nullptr) {
        throw Error::from_sdl("Could not acquire GPU command list");
    }
    return CommandList{*this, commands};
}

} // namespace mycore::render
