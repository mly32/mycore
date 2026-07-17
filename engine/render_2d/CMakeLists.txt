include(CompileShaders)

set(
    mycore_render_2d_shader_sources
    assets/shaders/grid.vert.hlsl
    assets/shaders/grid.frag.hlsl
    assets/shaders/circle.vert.hlsl
    assets/shaders/circle.frag.hlsl
)
list(TRANSFORM mycore_render_2d_shader_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

mycore_compile_hlsl_shaders(
    mycore_render_2d_shader_assets
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/assets/mycore/render_2d/shaders"
    SOURCES ${mycore_render_2d_shader_sources}
)

add_library(mycore_render_2d STATIC src/render_2d.cpp)
add_library(MyCore::Render2D ALIAS mycore_render_2d)
add_dependencies(mycore_render_2d mycore_render_2d_shader_assets)
set_target_properties(
    mycore_render_2d
    PROPERTIES
        MYCORE_RUNTIME_SHADER_ASSETS "${mycore_render_2d_shader_assets_OUTPUTS}"
        MYCORE_RUNTIME_SHADER_EXTENSION "${mycore_render_2d_shader_assets_EXTENSION}"
)

target_include_directories(
    mycore_render_2d
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    mycore_render_2d
    PUBLIC
        MyCore::Assets
        MyCore::Math
        MyCore::Render
)
mycore_configure_target(mycore_render_2d)
