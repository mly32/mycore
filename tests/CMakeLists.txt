find_package(Catch2 3 CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(mycore_core_smoke_tests mycore_core_smoke_tests.cpp)
target_link_libraries(
    mycore_core_smoke_tests
    PRIVATE
        MyCore::Core
        MyCore::Math
        MyCore::Time
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_core_smoke_tests)

include(Catch)
catch_discover_tests(mycore_core_smoke_tests)

add_executable(mycore_debug_tests mycore_debug_tests.cpp)
target_link_libraries(
    mycore_debug_tests
    PRIVATE
        MyCore::Debug
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_debug_tests)
catch_discover_tests(mycore_debug_tests)

add_executable(mycore_net_transport_tests mycore_net_transport_tests.cpp)
target_link_libraries(
    mycore_net_transport_tests
    PRIVATE
        MyCore::NetTransport
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_net_transport_tests)
mycore_stage_runtime_dependencies(mycore_net_transport_tests)
catch_discover_tests(mycore_net_transport_tests)

add_executable(mycore_assets_tests mycore_assets_tests.cpp)
target_link_libraries(
    mycore_assets_tests
    PRIVATE
        MyCore::Assets
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_assets_tests)
catch_discover_tests(mycore_assets_tests)

add_executable(mycore_rollback_tests mycore_rollback_tests.cpp)
target_link_libraries(
    mycore_rollback_tests
    PRIVATE
        MyCore::Rollback
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_rollback_tests)
catch_discover_tests(mycore_rollback_tests)

add_executable(mycore_platform_sdl_tests mycore_platform_sdl_tests.cpp)
target_link_libraries(
    mycore_platform_sdl_tests
    PRIVATE
        MyCore::PlatformSDL
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_platform_sdl_tests)
catch_discover_tests(
    mycore_platform_sdl_tests
    PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
)

add_executable(mycore_render_tests mycore_render_tests.cpp)
target_link_libraries(
    mycore_render_tests
    PRIVATE
        MyCore::Render
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_render_tests)
catch_discover_tests(mycore_render_tests)

add_executable(mycore_render_2d_tests mycore_render_2d_tests.cpp)
target_link_libraries(
    mycore_render_2d_tests
    PRIVATE
        MyCore::Render2D
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(mycore_render_2d_tests)
catch_discover_tests(mycore_render_2d_tests)

add_executable(dots_protocol_tests dots_protocol_tests.cpp)
target_link_libraries(
    dots_protocol_tests
    PRIVATE
        Dots::Protocol
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_protocol_tests)
catch_discover_tests(dots_protocol_tests)

add_executable(dots_app_cli_tests dots_app_cli_tests.cpp)
target_link_libraries(
    dots_app_cli_tests
    PRIVATE
        Dots::AppCli
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_app_cli_tests)
catch_discover_tests(dots_app_cli_tests)

add_executable(dots_replication_tests dots_replication_tests.cpp)
target_link_libraries(
    dots_replication_tests
    PRIVATE
        Dots::Replication
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_replication_tests)
catch_discover_tests(dots_replication_tests)

add_executable(dots_session_tests dots_prediction_tests.cpp dots_session_tests.cpp)
target_link_libraries(
    dots_session_tests
    PRIVATE
        Dots::ClientRuntime
        Dots::Prediction
        Dots::Server
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_session_tests)
mycore_stage_runtime_dependencies(dots_session_tests)
catch_discover_tests(dots_session_tests)

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
mycore_configure_test_executable(dots_simulation_tests)
catch_discover_tests(dots_simulation_tests)

add_executable(dots_world_prediction_tests dots_world_prediction_tests.cpp)
target_link_libraries(
    dots_world_prediction_tests
    PRIVATE
        Dots::Prediction
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_world_prediction_tests)
catch_discover_tests(dots_world_prediction_tests)

add_executable(dots_rollback_workload dots_rollback_workload.cpp)
target_link_libraries(
    dots_rollback_workload
    PRIVATE
        Dots::Prediction
)
mycore_configure_test_executable(dots_rollback_workload)
add_test(NAME dots_rollback_workload_verify COMMAND dots_rollback_workload --iterations 1)

add_executable(dots_presentation_tests dots_presentation_tests.cpp)
target_link_libraries(
    dots_presentation_tests
    PRIVATE
        Dots::Presentation
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_presentation_tests)
catch_discover_tests(dots_presentation_tests)

add_executable(dots_rollback_presentation_tests dots_rollback_presentation_tests.cpp)
target_link_libraries(
    dots_rollback_presentation_tests
    PRIVATE
        Dots::RollbackPresentation
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_rollback_presentation_tests)
catch_discover_tests(dots_rollback_presentation_tests)

add_executable(dots_remote_presentation_tests dots_remote_presentation_tests.cpp)
target_link_libraries(
    dots_remote_presentation_tests
    PRIVATE
        Dots::RemotePresentation
        Catch2::Catch2WithMain
)
mycore_configure_test_executable(dots_remote_presentation_tests)
catch_discover_tests(dots_remote_presentation_tests)

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
mycore_configure_test_executable(dots_client_support_tests)
catch_discover_tests(dots_client_support_tests)

add_test(NAME dots_client_help COMMAND dots_client --help)
set_tests_properties(
    dots_client_help
    PROPERTIES
        PASS_REGULAR_EXPRESSION "offline, embedded-authority, and native-network modes"
)

add_test(NAME dots_client_headless_smoke COMMAND dots_client --headless-smoke)
set_tests_properties(
    dots_client_headless_smoke
    PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
)

add_test(NAME dots_client_package_smoke COMMAND dots_client --package-smoke)
set_tests_properties(
    dots_client_package_smoke
    PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
)

add_test(NAME dots_server_help COMMAND dots_server --help)
set_tests_properties(dots_server_help PROPERTIES PASS_REGULAR_EXPRESSION "authoritative Dots")

add_test(NAME dots_bot_help COMMAND dots_bot --help)
set_tests_properties(dots_bot_help PROPERTIES PASS_REGULAR_EXPRESSION "wide rectangle")

add_test(
    NAME dots_server_ticks_smoke
    COMMAND dots_server --listen 127.0.0.1:0 --ticks 2 --respawn-cooldown-ticks 0
)

add_test(
    NAME dots_session_launcher_tests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/dots_session_launcher_test.py"
)
set_tests_properties(
    dots_session_launcher_tests
    PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)
