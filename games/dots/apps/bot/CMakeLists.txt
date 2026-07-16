add_executable(dots_bot main.cpp)
target_link_libraries(dots_bot PRIVATE Dots::Simulation)
mycore_configure_executable(dots_bot)
