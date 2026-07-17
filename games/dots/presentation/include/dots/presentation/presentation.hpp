#pragma once

#include "dots/simulation/ids.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/assets/directory_source.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/render/render.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dots::presentation {

enum class CircleKind {
    Food,
    Player,
};

struct CircleInstance {
    simulation::EntityId entity_id;
    mycore::math::Vector2 position;
    float radius{};
    CircleKind kind{};

    auto operator<=>(const CircleInstance&) const = default;
};

struct FrameData {
    mycore::math::Vector2 camera;
    std::vector<CircleInstance> circles;
};

struct Settings {
    float pixels_per_world_unit{20.0F};
    bool draw_grid{true};
    float grid_spacing_world_units{8.0F};
    mycore::render::Color background{0.063F, 0.094F, 0.125F, 1.0F};
    mycore::render::Color grid{0.125F, 0.188F, 0.251F, 1.0F};
    mycore::render::Color player{0.298F, 0.788F, 0.941F, 1.0F};
    mycore::render::Color food{0.969F, 0.145F, 0.522F, 1.0F};
    std::string input_mode{"hybrid"};
};

[[nodiscard]] FrameData extract_frame(const simulation::World& world, mycore::math::Vector2 camera);

class Presenter {
public:
    Presenter(mycore::render::Device& device, const mycore::assets::DirectorySource& assets);
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) = delete;
    Presenter& operator=(Presenter&&) = delete;

    void render(const FrameData& frame, const Settings& settings);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dots::presentation
