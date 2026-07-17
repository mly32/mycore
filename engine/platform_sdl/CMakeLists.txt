# ImGui's SDL backends also discover SDL from a sibling directory. Promote the imported targets so
# the full engine graph shares one SDL target identity and CMake can de-duplicate its link entry.
find_package(SDL3 CONFIG REQUIRED GLOBAL)

add_library(
    mycore_platform_sdl STATIC
    src/input.cpp
    src/runtime.cpp
    src/window.cpp
)
add_library(MyCore::PlatformSDL ALIAS mycore_platform_sdl)

target_include_directories(
    mycore_platform_sdl
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(mycore_platform_sdl PUBLIC SDL3::SDL3)
mycore_configure_target(mycore_platform_sdl)
