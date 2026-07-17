#include "mycore/render_2d/render_2d.hpp"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

TEST_CASE("Render2D draw lists are ordinary client-owned data", "[render_2d]") {
    mycore::render_2d::DrawList draw_list{
        .camera = {.center = {3.0F, 4.0F}, .pixels_per_world_unit = 20.0F},
        .clear_color = {0.1F, 0.2F, 0.3F, 1.0F},
        .grid =
            mycore::render_2d::Grid{
                .spacing_world_units = 8.0F,
                .color = {0.2F, 0.3F, 0.4F, 1.0F},
            },
        .circles = {{
            .center = {-2.0F, 5.0F},
            .radius = 1.25F,
            .color = {0.8F, 0.1F, 0.4F, 1.0F},
        }},
    };

    const auto copy = draw_list;

    REQUIRE(copy.camera == draw_list.camera);
    REQUIRE(copy.clear_color == draw_list.clear_color);
    REQUIRE(copy.grid == draw_list.grid);
    REQUIRE(copy.circles == draw_list.circles);
}

static_assert(std::is_copy_constructible_v<mycore::render_2d::DrawList>);
static_assert(std::is_move_constructible_v<mycore::render_2d::DrawList>);
static_assert(!std::is_copy_constructible_v<mycore::render_2d::Renderer>);
static_assert(!std::is_move_constructible_v<mycore::render_2d::Renderer>);
