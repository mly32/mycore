find_package(imgui CONFIG REQUIRED)

add_library(mycore_debug_ui STATIC src/context.cpp src/widgets.cpp)
add_library(MyCore::DebugUI ALIAS mycore_debug_ui)

target_include_directories(
    mycore_debug_ui
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    mycore_debug_ui
    PUBLIC
        MyCore::Debug
        MyCore::PlatformSDL
        MyCore::Render
        imgui::imgui
)
mycore_configure_target(mycore_debug_ui)
