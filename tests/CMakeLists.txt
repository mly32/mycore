find_package(Catch2 3 CONFIG REQUIRED)

add_executable(mycore_core_smoke_tests mycore_core_smoke_tests.cpp)
target_link_libraries(
    mycore_core_smoke_tests
    PRIVATE
        MyCore::Core
        MyCore::Math
        MyCore::Time
        Catch2::Catch2WithMain
)
mycore_configure_executable(mycore_core_smoke_tests)

include(Catch)
catch_discover_tests(mycore_core_smoke_tests)

add_executable(mycore_assets_tests mycore_assets_tests.cpp)
target_link_libraries(
    mycore_assets_tests
    PRIVATE
        MyCore::Assets
        Catch2::Catch2WithMain
)
mycore_configure_executable(mycore_assets_tests)
catch_discover_tests(mycore_assets_tests)

add_executable(mycore_platform_sdl_tests mycore_platform_sdl_tests.cpp)
target_link_libraries(
    mycore_platform_sdl_tests
    PRIVATE
        MyCore::PlatformSDL
        Catch2::Catch2WithMain
)
mycore_configure_executable(mycore_platform_sdl_tests)
catch_discover_tests(mycore_platform_sdl_tests)

add_executable(mycore_render_tests mycore_render_tests.cpp)
target_link_libraries(
    mycore_render_tests
    PRIVATE
        MyCore::Render
        Catch2::Catch2WithMain
)
mycore_configure_executable(mycore_render_tests)
catch_discover_tests(mycore_render_tests)

add_executable(mycore_render_2d_tests mycore_render_2d_tests.cpp)
target_link_libraries(
    mycore_render_2d_tests
    PRIVATE
        MyCore::Render2D
        Catch2::Catch2WithMain
)
mycore_configure_executable(mycore_render_2d_tests)
catch_discover_tests(mycore_render_2d_tests)

add_executable(
    dots_simulation_tests
    dots_collision_tests.cpp
    dots_simulation_tests.cpp
    dots_spatial_grid_tests.cpp
)
target_link_libraries(
    dots_simulation_tests
    PRIVATE
        Dots::Simulation
        Catch2::Catch2WithMain
)
mycore_configure_executable(dots_simulation_tests)
catch_discover_tests(dots_simulation_tests)

add_executable(dots_presentation_tests dots_presentation_tests.cpp)
target_link_libraries(
    dots_presentation_tests
    PRIVATE
        Dots::Presentation
        Catch2::Catch2WithMain
)
mycore_configure_executable(dots_presentation_tests)
catch_discover_tests(dots_presentation_tests)

add_executable(
    dots_client_support_tests
    dots_client_config_tests.cpp
    dots_client_controls_tests.cpp
)
target_link_libraries(
    dots_client_support_tests
    PRIVATE
        dots_client_support
        Catch2::Catch2WithMain
)
mycore_configure_executable(dots_client_support_tests)
catch_discover_tests(dots_client_support_tests)

add_test(NAME dots_client_help COMMAND dots_client --help)
set_tests_properties(
    dots_client_help
    PROPERTIES
        PASS_REGULAR_EXPRESSION "playable offline SDL_GPU client"
)

add_test(NAME dots_client_headless_smoke COMMAND dots_client --headless-smoke)
set_tests_properties(
    dots_client_headless_smoke
    PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
)
