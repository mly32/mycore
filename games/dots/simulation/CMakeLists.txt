add_library(
    dots_simulation STATIC
    src/movement.cpp
    src/simulation.cpp
    src/spatial_grid.cpp
    src/tick.cpp
    src/world.cpp
    src/world_setup.cpp
)
add_library(Dots::Simulation ALIAS dots_simulation)

target_include_directories(
    dots_simulation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_simulation
    PUBLIC
        MyCore::Core
        MyCore::Math
        MyCore::Time
)
mycore_configure_target(dots_simulation)
