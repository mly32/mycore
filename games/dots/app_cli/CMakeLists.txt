add_library(dots_app_cli STATIC src/app_cli.cpp)
add_library(Dots::AppCli ALIAS dots_app_cli)

target_include_directories(
    dots_app_cli
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(dots_app_cli PUBLIC MyCore::NetTransport)
mycore_configure_target(dots_app_cli)
