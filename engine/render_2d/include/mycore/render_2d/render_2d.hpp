#pragma once

#include "mycore/assets/directory_source.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/render/render.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace mycore::render_2d {

struct Camera {
    math::Vector2 center;
    float pixels_per_world_unit{1.0F};

    auto operator<=>(const Camera&) const = default;
};

struct Circle {
    math::Vector2 center;
    float radius{};
    render::Color color;

    auto operator<=>(const Circle&) const = default;
};

struct Grid {
    float spacing_world_units{1.0F};
    render::Color color;

    auto operator<=>(const Grid&) const = default;
};

struct DrawList {
    Camera camera;
    render::Color clear_color;
    std::optional<Grid> grid;
    std::vector<Circle> circles;
};

class Renderer {
public:
    Renderer(render::Device& device, const assets::DirectorySource& assets);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void render(const DrawList& draw_list);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mycore::render_2d
