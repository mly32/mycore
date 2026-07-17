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
