#include "mycore/render/render.hpp"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

TEST_CASE("Render shader metadata follows the target platform", "[render]") {
    const auto format = mycore::render::platform_shader_format();

    REQUIRE_FALSE(mycore::render::shader_file_extension(format).empty());
    REQUIRE_FALSE(mycore::render::shader_entry_point(format).empty());
}

TEST_CASE("Render present mode respects vsync and supported fallbacks", "[render]") {
    using mycore::render::choose_present_mode;
    using mycore::render::PresentMode;

    REQUIRE(choose_present_mode(true, true, true) == PresentMode::Vsync);
    REQUIRE(choose_present_mode(false, true, true) == PresentMode::Immediate);
    REQUIRE(choose_present_mode(false, false, true) == PresentMode::Mailbox);
    REQUIRE(choose_present_mode(false, false, false) == PresentMode::Vsync);
}

static_assert(!std::is_copy_constructible_v<mycore::render::Device>);
static_assert(!std::is_move_constructible_v<mycore::render::Device>);
static_assert(!std::is_copy_constructible_v<mycore::render::Buffer>);
static_assert(std::is_nothrow_move_constructible_v<mycore::render::Buffer>);
static_assert(!std::is_copy_constructible_v<mycore::render::Shader>);
static_assert(std::is_nothrow_move_constructible_v<mycore::render::Shader>);
static_assert(!std::is_copy_constructible_v<mycore::render::GraphicsPipeline>);
static_assert(std::is_nothrow_move_constructible_v<mycore::render::GraphicsPipeline>);
