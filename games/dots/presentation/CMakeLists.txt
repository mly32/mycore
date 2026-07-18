add_library(dots_presentation STATIC src/presentation.cpp)
add_library(Dots::Presentation ALIAS dots_presentation)

target_include_directories(
    dots_presentation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_presentation
    PUBLIC
        Dots::Replication
        Dots::Simulation
        MyCore::Render2D
)
mycore_configure_target(dots_presentation)
