add_library(mycore_render STATIC src/render.cpp)
add_library(MyCore::Render ALIAS mycore_render)

target_include_directories(
    mycore_render
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    mycore_render
    PUBLIC
        MyCore::Core
        MyCore::PlatformSDL
)
mycore_configure_target(mycore_render)
