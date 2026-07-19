find_package(tomlplusplus CONFIG REQUIRED)

set(
    dots_client_config_schema
    "${CMAKE_SOURCE_DIR}/games/dots/config/dots-client.schema.json"
)
# Parse the checked-in schema during every configure so a malformed edit fails before packaging.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dots_client_config_schema}")
file(READ "${dots_client_config_schema}" dots_client_config_schema_json)
string(JSON dots_client_config_schema_type TYPE "${dots_client_config_schema_json}")
if(NOT dots_client_config_schema_type STREQUAL "OBJECT")
    message(FATAL_ERROR "Dots client configuration schema must contain a JSON object")
endif()

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
        MyCore::NetTransport
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
        Dots::ClientRuntime
        Dots::Presentation
        Dots::Server
        MyCore::DebugUI
        MyCore::NetTransport
        MyCore::Render2D
)
mycore_configure_executable(dots_client)
mycore_stage_runtime_dependencies(dots_client)

get_target_property(
    dots_client_shader_assets
    mycore_render_2d
    MYCORE_RUNTIME_SHADER_ASSETS
)
if(NOT dots_client_shader_assets)
    message(FATAL_ERROR "MyCore::Render2D did not declare its runtime shader assets")
endif()

set(dots_client_install_executable_directory ".")
set(dots_client_install_resource_directory ".")
set(dots_client_install_shader_directory "assets/mycore/render_2d/shaders")
if(APPLE)
    set(dots_client_install_executable_directory "Dots.app/Contents/MacOS")
    set(dots_client_install_resource_directory "Dots.app/Contents/Resources")
    set(
        dots_client_install_shader_directory
        "Dots.app/Contents/Resources/assets/mycore/render_2d/shaders"
    )
    configure_file(
        DotsInfo.plist.in
        "${CMAKE_CURRENT_BINARY_DIR}/DotsInfo.plist"
        @ONLY
    )
    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/DotsInfo.plist"
        DESTINATION "Dots.app/Contents"
        RENAME Info.plist
        COMPONENT DotsClient
    )
endif()

install(
    TARGETS dots_client
    RUNTIME DESTINATION "${dots_client_install_executable_directory}"
    COMPONENT DotsClient
)

install(
    FILES ${dots_client_shader_assets}
    DESTINATION "${dots_client_install_shader_directory}"
    COMPONENT DotsClient
)
install(
    FILES "${CMAKE_SOURCE_DIR}/games/dots/config/dots-client.toml"
    DESTINATION "${dots_client_install_resource_directory}"
    RENAME dots-client.example.toml
    COMPONENT DotsClient
)
install(
    FILES "${dots_client_config_schema}"
    DESTINATION "${dots_client_install_resource_directory}"
    COMPONENT DotsClient
)
