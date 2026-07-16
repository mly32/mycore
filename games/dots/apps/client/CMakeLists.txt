add_executable(dots_client main.cpp)
target_link_libraries(dots_client PRIVATE Dots::Simulation)
mycore_configure_executable(dots_client)
