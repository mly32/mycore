add_library(dots_simulation STATIC src/simulation.cpp)
add_library(Dots::Simulation ALIAS dots_simulation)

target_include_directories(
    dots_simulation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(dots_simulation PUBLIC MyCore::Core)
mycore_configure_target(dots_simulation)

