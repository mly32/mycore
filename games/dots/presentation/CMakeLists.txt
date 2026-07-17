include(CompileShaders)

set(
    dots_shader_sources
    assets/shaders/grid.vert.hlsl
    assets/shaders/grid.frag.hlsl
    assets/shaders/circle.vert.hlsl
    assets/shaders/circle.frag.hlsl
    assets/shaders/solid.vert.hlsl
    assets/shaders/solid.frag.hlsl
)
list(TRANSFORM dots_shader_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../")

mycore_compile_hlsl_shaders(
    dots_shader_assets
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/assets/dots/shaders"
    SOURCES ${dots_shader_sources}
)

add_library(dots_presentation STATIC src/presentation.cpp)
add_library(Dots::Presentation ALIAS dots_presentation)
add_dependencies(dots_presentation dots_shader_assets)

target_include_directories(
    dots_presentation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_presentation
    PUBLIC
        Dots::Simulation
        MyCore::Assets
        MyCore::Render
)
mycore_configure_target(dots_presentation)
