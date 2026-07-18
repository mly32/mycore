add_library(dots_server_runtime STATIC src/server_runtime.cpp)
add_library(Dots::Server ALIAS dots_server_runtime)

target_include_directories(
    dots_server_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_server_runtime
    PUBLIC
        Dots::Protocol
        Dots::Replication
        Dots::Simulation
        MyCore::NetTransport
)
mycore_configure_target(dots_server_runtime)
