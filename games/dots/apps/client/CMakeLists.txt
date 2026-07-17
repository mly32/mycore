find_package(tomlplusplus CONFIG REQUIRED)

add_library(
    dots_client_support STATIC
    src/client_config.cpp
    src/controls.cpp
)
target_include_directories(
    dots_client_support
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    dots_client_support
    PUBLIC
        Dots::Simulation
        MyCore::PlatformSDL
    PRIVATE
        tomlplusplus::tomlplusplus
)
mycore_configure_target(dots_client_support)

add_executable(dots_client main.cpp src/client_app.cpp)
target_link_libraries(
    dots_client
    PRIVATE
        dots_client_support
        Dots::Presentation
        MyCore::Render2D
)
mycore_configure_executable(dots_client)
