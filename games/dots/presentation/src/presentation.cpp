#include "dots/presentation/presentation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace dots::presentation {
namespace {

using mycore::render::Buffer;
using mycore::render::Color;
using mycore::render::GraphicsPipeline;
using mycore::render::Shader;

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
    float padding;
    float red;
    float green;
    float blue;
    float alpha;
};

struct SolidGpuInstance {
    float center_x;
    float center_y;
    float half_width;
    float half_height;
    float red;
    float green;
    float blue;
    float alpha;
};

struct alignas(16) GridUniforms {
    std::array<float, 4> camera_and_output_width;
    std::array<float, 4> output_height_scale_spacing_enabled;
    std::array<float, 4> background_color;
    std::array<float, 4> grid_color;
};

struct alignas(16) CircleViewUniforms {
    std::array<float, 4> camera_and_output_width;
    std::array<float, 4> output_height_and_scale;
};

struct alignas(16) SolidViewUniforms {
    std::array<float, 4> output_size;
};

static_assert(sizeof(CircleGpuInstance) == 32);
static_assert(sizeof(SolidGpuInstance) == 32);
static_assert(sizeof(GridUniforms) == 64);
static_assert(sizeof(CircleViewUniforms) == 32);
static_assert(sizeof(SolidViewUniforms) == 16);

[[nodiscard]] std::array<float, 4> components(Color color) {
    return {color.red, color.green, color.blue, color.alpha};
}

[[nodiscard]] std::string shader_asset_name(std::string_view name,
                                            mycore::render::ShaderFormat format) {
    return "shaders/" + std::string{name} + "." +
           std::string{mycore::render::shader_file_extension(format)};
}

[[nodiscard]] Shader load_shader(mycore::render::Device& device,
                                 const mycore::assets::DirectorySource& assets,
                                 std::string_view name,
                                 mycore::render::ShaderStage stage,
                                 std::uint32_t uniform_count) {
    const auto path = shader_asset_name(name, device.shader_format());
    const auto bytes = assets.read(path);
    return device.create_shader(
        bytes,
        {
            .stage = stage,
            .entry_point = mycore::render::shader_entry_point(device.shader_format()),
            .uniform_buffer_count = uniform_count,
            .label = name,
        });
}

[[nodiscard]] GraphicsPipeline
create_grid_pipeline(mycore::render::Device& device, const Shader& vertex, const Shader& fragment) {
    return device.create_graphics_pipeline({
        .vertex_shader = &vertex,
        .fragment_shader = &fragment,
        .label = "Dots grid pipeline",
    });
}

[[nodiscard]] GraphicsPipeline create_circle_pipeline(mycore::render::Device& device,
                                                      const Shader& vertex,
                                                      const Shader& fragment) {
    constexpr std::array layouts{
        mycore::render::VertexBufferLayout{
            .slot = 0,
            .stride = sizeof(float) * 2,
        },
        mycore::render::VertexBufferLayout{
            .slot = 1,
            .stride = sizeof(CircleGpuInstance),
            .input_rate = mycore::render::VertexInputRate::Instance,
        },
    };
    constexpr std::array attributes{
        mycore::render::VertexAttribute{
            .location = 0,
            .buffer_slot = 0,
            .format = mycore::render::VertexFormat::Float2,
            .offset = 0,
        },
        mycore::render::VertexAttribute{
            .location = 1,
            .buffer_slot = 1,
            .format = mycore::render::VertexFormat::Float2,
            .offset = offsetof(CircleGpuInstance, center_x),
        },
        mycore::render::VertexAttribute{
            .location = 2,
            .buffer_slot = 1,
            .format = mycore::render::VertexFormat::Float,
            .offset = offsetof(CircleGpuInstance, radius),
        },
        mycore::render::VertexAttribute{
            .location = 3,
            .buffer_slot = 1,
            .format = mycore::render::VertexFormat::Float4,
            .offset = offsetof(CircleGpuInstance, red),
        },
    };
    return device.create_graphics_pipeline({
        .vertex_shader = &vertex,
        .fragment_shader = &fragment,
        .vertex_buffers = layouts,
        .vertex_attributes = attributes,
        .enable_blending = true,
        .label = "Dots circle pipeline",
    });
}

[[nodiscard]] GraphicsPipeline create_solid_pipeline(mycore::render::Device& device,
                                                     const Shader& vertex,
                                                     const Shader& fragment) {
    constexpr std::array layouts{
        mycore::render::VertexBufferLayout{
            .slot = 0,
            .stride = sizeof(float) * 2,
        },
        mycore::render::VertexBufferLayout{
            .slot = 1,
            .stride = sizeof(SolidGpuInstance),
            .input_rate = mycore::render::VertexInputRate::Instance,
        },
    };
    constexpr std::array attributes{
        mycore::render::VertexAttribute{
            .location = 0,
            .buffer_slot = 0,
            .format = mycore::render::VertexFormat::Float2,
            .offset = 0,
        },
        mycore::render::VertexAttribute{
            .location = 1,
            .buffer_slot = 1,
            .format = mycore::render::VertexFormat::Float2,
            .offset = offsetof(SolidGpuInstance, center_x),
        },
        mycore::render::VertexAttribute{
            .location = 2,
            .buffer_slot = 1,
            .format = mycore::render::VertexFormat::Float2,
            .offset = offsetof(SolidGpuInstance, half_width),
        },
        mycore::render::VertexAttribute{
            .location = 3,
            .buffer_slot = 1,
            .format = mycore::render::VertexFormat::Float4,
            .offset = offsetof(SolidGpuInstance, red),
        },
    };
    return device.create_graphics_pipeline({
        .vertex_shader = &vertex,
        .fragment_shader = &fragment,
        .vertex_buffers = layouts,
        .vertex_attributes = attributes,
        .enable_blending = true,
        .label = "Dots HUD pipeline",
    });
}

[[nodiscard]] std::array<std::uint8_t, 7> glyph(char character) {
    switch (character) {
    case 'A':
        return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B':
        return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'D':
        return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E':
        return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'H':
        return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I':
        return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'K':
        return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'M':
        return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N':
        return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O':
        return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P':
        return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'R':
        return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S':
        return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T':
        return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U':
        return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'Y':
        return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case ':':
        return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    default:
        return {};
    }
}

void append_solid(std::vector<SolidGpuInstance>& instances,
                  float left,
                  float top,
                  float width,
                  float height,
                  Color color) {
    instances.push_back({
        .center_x = left + (width * 0.5F),
        .center_y = top + (height * 0.5F),
        .half_width = width * 0.5F,
        .half_height = height * 0.5F,
        .red = color.red,
        .green = color.green,
        .blue = color.blue,
        .alpha = color.alpha,
    });
}

[[nodiscard]] std::vector<SolidGpuInstance>
make_hud(std::uint32_t output_width, std::uint32_t output_height, const Settings& settings) {
    constexpr float pixel = 2.0F;
    constexpr float glyph_advance = 6.0F * pixel;
    constexpr float padding = 6.0F;
    constexpr float margin = 6.0F;
    const auto label = std::string{"INPUT: "} + settings.input_mode;
    const auto panel_width = (static_cast<float>(label.size()) * glyph_advance) + (padding * 2.0F);
    constexpr float panel_height = (7.0F * pixel) + (padding * 2.0F);
    const auto panel_left = std::max(0.0F, static_cast<float>(output_width) - panel_width - margin);
    const auto panel_top =
        std::max(0.0F, static_cast<float>(output_height) - panel_height - margin);

    std::vector<SolidGpuInstance> instances;
    instances.reserve(1 + (label.size() * 20));
    append_solid(instances, panel_left, panel_top, panel_width, panel_height, settings.grid);

    auto cursor_x = panel_left + padding;
    const auto glyph_top = panel_top + padding;
    for (const auto raw_character : label) {
        const auto character = static_cast<char>(raw_character >= 'a' && raw_character <= 'z'
                                                     ? raw_character - ('a' - 'A')
                                                     : raw_character);
        const auto rows = glyph(character);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (std::size_t column = 0; column < 5; ++column) {
                const auto mask = static_cast<std::uint8_t>(1U << (4U - column));
                if ((rows[row] & mask) == 0) {
                    continue;
                }
                append_solid(instances,
                             cursor_x + (static_cast<float>(column) * pixel),
                             glyph_top + (static_cast<float>(row) * pixel),
                             pixel,
                             pixel,
                             settings.player);
            }
        }
        cursor_x += glyph_advance;
    }
    return instances;
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

FrameData extract_frame(const simulation::World& world, mycore::math::Vector2 camera) {
    FrameData frame{.camera = camera};
    frame.circles.reserve(world.food_count() + world.player_count());

    const auto append_entities = [&world, &frame](std::span<const simulation::EntityId> ids,
                                                  CircleKind kind) {
        for (const auto entity_id : ids) {
            const auto position = world.position(entity_id);
            const auto radius = world.radius(entity_id);
            if (!position || !radius || !std::isfinite(position->x) ||
                !std::isfinite(position->y) || !std::isfinite(*radius) || *radius <= 0.0F) {
                throw std::runtime_error{"Dots presentation encountered invalid entity geometry"};
            }
            frame.circles.push_back({
                .entity_id = entity_id,
                .position = *position,
                .radius = *radius,
                .kind = kind,
            });
        }
    };

    append_entities(world.food_ids(), CircleKind::Food);
    append_entities(world.player_ids(), CircleKind::Player);
    return frame;
}

class Presenter::Impl {
public:
    Impl(mycore::render::Device& device, const mycore::assets::DirectorySource& assets)
        : device_(device),
          grid_vertex_(
              load_shader(device, assets, "grid.vert", mycore::render::ShaderStage::Vertex, 0)),
          grid_fragment_(
              load_shader(device, assets, "grid.frag", mycore::render::ShaderStage::Fragment, 1)),
          circle_vertex_(
              load_shader(device, assets, "circle.vert", mycore::render::ShaderStage::Vertex, 1)),
          circle_fragment_(
              load_shader(device, assets, "circle.frag", mycore::render::ShaderStage::Fragment, 0)),
          solid_vertex_(
              load_shader(device, assets, "solid.vert", mycore::render::ShaderStage::Vertex, 1)),
          solid_fragment_(
              load_shader(device, assets, "solid.frag", mycore::render::ShaderStage::Fragment, 0)),
          grid_pipeline_(create_grid_pipeline(device, grid_vertex_, grid_fragment_)),
          circle_pipeline_(create_circle_pipeline(device, circle_vertex_, circle_fragment_)),
          solid_pipeline_(create_solid_pipeline(device, solid_vertex_, solid_fragment_)),
          quad_buffer_(device.create_buffer({
              .size = sizeof(kQuadVertices),
              .label = "Dots unit quad",
          })) {
        auto commands = device_.acquire_command_list();
        commands.upload(quad_buffer_, std::as_bytes(std::span{kQuadVertices}));
        commands.submit();
    }

    void render(const FrameData& frame, const Settings& settings) {
        std::vector<CircleGpuInstance> circles;
        circles.reserve(frame.circles.size());
        for (const auto& circle : frame.circles) {
            const auto color = circle.kind == CircleKind::Food ? settings.food : settings.player;
            circles.push_back({
                .center_x = circle.position.x,
                .center_y = circle.position.y,
                .radius = circle.radius,
                .padding = 0.0F,
                .red = color.red,
                .green = color.green,
                .blue = color.blue,
                .alpha = color.alpha,
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
            return;
        }

        auto solids = make_hud(target.width(), target.height(), settings);
        ensure_solid_capacity(solids.size());
        if (!solids.empty()) {
            commands.upload(solid_buffer_, std::as_bytes(std::span{solids}));
        }

        const auto output_width = static_cast<float>(target.width());
        const auto output_height = static_cast<float>(target.height());
        const GridUniforms grid_uniforms{
            .camera_and_output_width = {frame.camera.x, frame.camera.y, output_width, 0.0F},
            .output_height_scale_spacing_enabled =
                {
                    output_height,
                    settings.pixels_per_world_unit,
                    settings.grid_spacing_world_units,
                    settings.draw_grid ? 1.0F : 0.0F,
                },
            .background_color = components(settings.background),
            .grid_color = components(settings.grid),
        };
        const CircleViewUniforms circle_uniforms{
            .camera_and_output_width = {frame.camera.x, frame.camera.y, output_width, 0.0F},
            .output_height_and_scale =
                {
                    output_height,
                    settings.pixels_per_world_unit,
                    0.0F,
                    0.0F,
                },
        };
        const SolidViewUniforms solid_uniforms{
            .output_size = {output_width, output_height, 0.0F, 0.0F},
        };

        auto pass = commands.begin_render_pass(target, settings.background);
        pass.bind_pipeline(grid_pipeline_);
        commands.push_fragment_uniform(0, grid_uniforms);
        pass.draw(3);

        if (!circles.empty()) {
            pass.bind_pipeline(circle_pipeline_);
            const std::array bindings{
                mycore::render::BufferBinding{.buffer = &quad_buffer_},
                mycore::render::BufferBinding{.buffer = &circle_buffer_},
            };
            pass.bind_vertex_buffers(0, bindings);
            commands.push_vertex_uniform(0, circle_uniforms);
            pass.draw(6, static_cast<std::uint32_t>(circles.size()));
        }

        if (!solids.empty()) {
            pass.bind_pipeline(solid_pipeline_);
            const std::array bindings{
                mycore::render::BufferBinding{.buffer = &quad_buffer_},
                mycore::render::BufferBinding{.buffer = &solid_buffer_},
            };
            pass.bind_vertex_buffers(0, bindings);
            commands.push_vertex_uniform(0, solid_uniforms);
            pass.draw(6, static_cast<std::uint32_t>(solids.size()));
        }

        pass.end();
        commands.submit();
    }

private:
    void ensure_circle_capacity(std::size_t required) {
        if (required <= circle_capacity_) {
            return;
        }
        circle_capacity_ = next_capacity(circle_capacity_, required);
        circle_buffer_ = device_.create_buffer({
            .size = circle_capacity_ * sizeof(CircleGpuInstance),
            .label = "Dots circle instances",
        });
    }

    void ensure_solid_capacity(std::size_t required) {
        if (required <= solid_capacity_) {
            return;
        }
        solid_capacity_ = next_capacity(solid_capacity_, required);
        solid_buffer_ = device_.create_buffer({
            .size = solid_capacity_ * sizeof(SolidGpuInstance),
            .label = "Dots HUD instances",
        });
    }

    mycore::render::Device& device_;
    Shader grid_vertex_;
    Shader grid_fragment_;
    Shader circle_vertex_;
    Shader circle_fragment_;
    Shader solid_vertex_;
    Shader solid_fragment_;
    GraphicsPipeline grid_pipeline_;
    GraphicsPipeline circle_pipeline_;
    GraphicsPipeline solid_pipeline_;
    Buffer quad_buffer_;
    Buffer circle_buffer_;
    Buffer solid_buffer_;
    std::size_t circle_capacity_{};
    std::size_t solid_capacity_{};
};

Presenter::Presenter(mycore::render::Device& device, const mycore::assets::DirectorySource& assets)
    : impl_(std::make_unique<Impl>(device, assets)) {}

Presenter::~Presenter() = default;

void Presenter::render(const FrameData& frame, const Settings& settings) {
    impl_->render(frame, settings);
}

} // namespace dots::presentation
