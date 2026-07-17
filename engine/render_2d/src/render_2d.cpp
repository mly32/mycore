#include "mycore/render_2d/render_2d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mycore::render_2d {
namespace {

using render::Buffer;
using render::Color;
using render::GraphicsPipeline;
using render::Shader;

constexpr std::array<float, 12> kQuadVertices{
    -1.0F,
    -1.0F,
    1.0F,
    -1.0F,
    1.0F,
    1.0F,
    -1.0F,
    -1.0F,
    1.0F,
    1.0F,
    -1.0F,
    1.0F,
};

struct CircleGpuInstance {
    float center_x;
    float center_y;
    float radius;
    float outline_width_pixels;
    float red;
    float green;
    float blue;
    float alpha;
    float outline_red;
    float outline_green;
    float outline_blue;
    float outline_alpha;
};

struct alignas(16) GridUniforms {
    std::array<float, 4> camera_and_output_width;
    std::array<float, 4> output_height_scale_spacing;
    std::array<float, 4> background_color;
    std::array<float, 4> grid_color;
};

struct alignas(16) CircleViewUniforms {
    std::array<float, 4> camera_and_output_width;
    std::array<float, 4> output_height_and_scale;
};

static_assert(sizeof(CircleGpuInstance) == 48);
static_assert(sizeof(GridUniforms) == 64);
static_assert(sizeof(CircleViewUniforms) == 32);

[[nodiscard]] bool finite(Color color) noexcept {
    return std::isfinite(color.red) && std::isfinite(color.green) && std::isfinite(color.blue) &&
           std::isfinite(color.alpha);
}

void validate(const DrawList& draw_list) {
    if (!std::isfinite(draw_list.camera.center.x) || !std::isfinite(draw_list.camera.center.y) ||
        !std::isfinite(draw_list.camera.pixels_per_world_unit) ||
        draw_list.camera.pixels_per_world_unit <= 0.0F) {
        throw render::Error{"Render2D camera must be finite with a positive scale"};
    }
    if (!finite(draw_list.clear_color)) {
        throw render::Error{"Render2D clear color must be finite"};
    }
    if (draw_list.grid &&
        (!std::isfinite(draw_list.grid->spacing_world_units) ||
         draw_list.grid->spacing_world_units <= 0.0F || !finite(draw_list.grid->color))) {
        throw render::Error{"Render2D grid must have finite color and positive spacing"};
    }
    if (draw_list.circles.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw render::Error{"Render2D circle count exceeds the instanced draw limit"};
    }
    for (const auto& circle : draw_list.circles) {
        if (!std::isfinite(circle.center.x) || !std::isfinite(circle.center.y) ||
            !std::isfinite(circle.radius) || circle.radius <= 0.0F || !finite(circle.color) ||
            !finite(circle.outline_color) || !std::isfinite(circle.outline_width_pixels) ||
            circle.outline_width_pixels < 0.0F) {
            throw render::Error{
                "Render2D circle geometry, fill, and outline must be finite with a positive "
                "radius and non-negative outline width"};
        }
    }
}

[[nodiscard]] std::array<float, 4> components(Color color) {
    return {color.red, color.green, color.blue, color.alpha};
}

[[nodiscard]] std::string shader_asset_name(std::string_view name, render::ShaderFormat format) {
    return "mycore/render_2d/shaders/" + std::string{name} + "." +
           std::string{render::shader_file_extension(format)};
}

[[nodiscard]] Shader load_shader(render::Device& device,
                                 const assets::DirectorySource& assets,
                                 std::string_view name,
                                 render::ShaderStage stage,
                                 std::uint32_t uniform_count) {
    const auto shader_path = shader_asset_name(name, device.shader_format());
    const auto bytes = assets.read(shader_path);
    return device.create_shader(
        bytes,
        {
            .stage = stage,
            .entry_point = render::shader_entry_point(device.shader_format()),
            .uniform_buffer_count = uniform_count,
            .label = name,
        });
}

[[nodiscard]] GraphicsPipeline
create_grid_pipeline(render::Device& device, const Shader& vertex, const Shader& fragment) {
    return device.create_graphics_pipeline({
        .vertex_shader = &vertex,
        .fragment_shader = &fragment,
        .label = "MyCore Render2D grid pipeline",
    });
}

[[nodiscard]] GraphicsPipeline
create_circle_pipeline(render::Device& device, const Shader& vertex, const Shader& fragment) {
    constexpr std::array layouts{
        render::VertexBufferLayout{
            .slot = 0,
            .stride = sizeof(float) * 2,
        },
        render::VertexBufferLayout{
            .slot = 1,
            .stride = sizeof(CircleGpuInstance),
            .input_rate = render::VertexInputRate::Instance,
        },
    };
    constexpr std::array attributes{
        render::VertexAttribute{
            .location = 0,
            .buffer_slot = 0,
            .format = render::VertexFormat::Float2,
            .offset = 0,
        },
        render::VertexAttribute{
            .location = 1,
            .buffer_slot = 1,
            .format = render::VertexFormat::Float2,
            .offset = offsetof(CircleGpuInstance, center_x),
        },
        render::VertexAttribute{
            .location = 2,
            .buffer_slot = 1,
            .format = render::VertexFormat::Float,
            .offset = offsetof(CircleGpuInstance, radius),
        },
        render::VertexAttribute{
            .location = 3,
            .buffer_slot = 1,
            .format = render::VertexFormat::Float4,
            .offset = offsetof(CircleGpuInstance, red),
        },
        render::VertexAttribute{
            .location = 4,
            .buffer_slot = 1,
            .format = render::VertexFormat::Float4,
            .offset = offsetof(CircleGpuInstance, outline_red),
        },
        render::VertexAttribute{
            .location = 5,
            .buffer_slot = 1,
            .format = render::VertexFormat::Float,
            .offset = offsetof(CircleGpuInstance, outline_width_pixels),
        },
    };
    return device.create_graphics_pipeline({
        .vertex_shader = &vertex,
        .fragment_shader = &fragment,
        .vertex_buffers = layouts,
        .vertex_attributes = attributes,
        .enable_blending = true,
        .label = "MyCore Render2D circle pipeline",
    });
}

[[nodiscard]] std::size_t next_capacity(std::size_t current, std::size_t required) {
    auto capacity = std::max<std::size_t>(current, 1);
    while (capacity < required) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
            return required;
        }
        capacity *= 2;
    }
    return capacity;
}

} // namespace

class Renderer::Impl {
public:
    Impl(render::Device& device, const assets::DirectorySource& assets)
        : device_(device),
          grid_vertex_(load_shader(device, assets, "grid.vert", render::ShaderStage::Vertex, 0)),
          grid_fragment_(
              load_shader(device, assets, "grid.frag", render::ShaderStage::Fragment, 1)),
          circle_vertex_(
              load_shader(device, assets, "circle.vert", render::ShaderStage::Vertex, 1)),
          circle_fragment_(
              load_shader(device, assets, "circle.frag", render::ShaderStage::Fragment, 0)),
          grid_pipeline_(create_grid_pipeline(device, grid_vertex_, grid_fragment_)),
          circle_pipeline_(create_circle_pipeline(device, circle_vertex_, circle_fragment_)),
          quad_buffer_(device.create_buffer({
              .size = sizeof(kQuadVertices),
              .label = "MyCore Render2D unit quad",
          })) {
        auto commands = device_.acquire_command_list();
        commands.upload(quad_buffer_, std::as_bytes(std::span{kQuadVertices}));
        commands.submit();
    }

    bool render(const DrawList& draw_list, const Renderer::FrameExtension& extension) {
        validate(draw_list);

        std::vector<CircleGpuInstance> circles;
        circles.reserve(draw_list.circles.size());
        for (const auto& circle : draw_list.circles) {
            const auto outline =
                circle.outline_width_pixels > 0.0F ? circle.outline_color : circle.color;
            circles.push_back({
                .center_x = circle.center.x,
                .center_y = circle.center.y,
                .radius = circle.radius,
                .outline_width_pixels = circle.outline_width_pixels,
                .red = circle.color.red,
                .green = circle.color.green,
                .blue = circle.color.blue,
                .alpha = circle.color.alpha,
                .outline_red = outline.red,
                .outline_green = outline.green,
                .outline_blue = outline.blue,
                .outline_alpha = outline.alpha,
            });
        }

        ensure_circle_capacity(circles.size());
        auto commands = device_.acquire_command_list();
        if (!circles.empty()) {
            commands.upload(circle_buffer_, std::as_bytes(std::span{circles}));
        }

        auto target = commands.acquire_swapchain();
        if (!target) {
            commands.submit();
            return false;
        }

        const auto output_width = static_cast<float>(target.width());
        const auto output_height = static_cast<float>(target.height());
        const CircleViewUniforms circle_uniforms{
            .camera_and_output_width = {draw_list.camera.center.x,
                                        draw_list.camera.center.y,
                                        output_width,
                                        0.0F},
            .output_height_and_scale = {output_height,
                                        draw_list.camera.pixels_per_world_unit,
                                        0.0F,
                                        0.0F},
        };

        auto pass = commands.begin_render_pass(target, draw_list.clear_color);
        if (draw_list.grid) {
            const GridUniforms grid_uniforms{
                .camera_and_output_width = {draw_list.camera.center.x,
                                            draw_list.camera.center.y,
                                            output_width,
                                            0.0F},
                .output_height_scale_spacing = {output_height,
                                                draw_list.camera.pixels_per_world_unit,
                                                draw_list.grid->spacing_world_units,
                                                0.0F},
                .background_color = components(draw_list.clear_color),
                .grid_color = components(draw_list.grid->color),
            };
            pass.bind_pipeline(grid_pipeline_);
            commands.push_fragment_uniform(0, grid_uniforms);
            pass.draw(3);
        }

        if (!circles.empty()) {
            pass.bind_pipeline(circle_pipeline_);
            const std::array bindings{
                render::BufferBinding{.buffer = &quad_buffer_},
                render::BufferBinding{.buffer = &circle_buffer_},
            };
            pass.bind_vertex_buffers(0, bindings);
            commands.push_vertex_uniform(0, circle_uniforms);
            pass.draw(6, static_cast<std::uint32_t>(circles.size()));
        }

        pass.end();
        if (extension) {
            extension(commands, target);
        }
        commands.submit();
        return true;
    }

private:
    void ensure_circle_capacity(std::size_t required) {
        if (required <= circle_capacity_) {
            return;
        }
        circle_capacity_ = next_capacity(circle_capacity_, required);
        circle_buffer_ = device_.create_buffer({
            .size = circle_capacity_ * sizeof(CircleGpuInstance),
            .label = "MyCore Render2D circle instances",
        });
    }

    render::Device& device_;
    Shader grid_vertex_;
    Shader grid_fragment_;
    Shader circle_vertex_;
    Shader circle_fragment_;
    GraphicsPipeline grid_pipeline_;
    GraphicsPipeline circle_pipeline_;
    Buffer quad_buffer_;
    Buffer circle_buffer_;
    std::size_t circle_capacity_{};
};

Renderer::Renderer(render::Device& device, const assets::DirectorySource& assets)
    : impl_(std::make_unique<Impl>(device, assets)) {}

Renderer::~Renderer() = default;

bool Renderer::render(const DrawList& draw_list, const FrameExtension& extension) {
    return impl_->render(draw_list, extension);
}

} // namespace mycore::render_2d
