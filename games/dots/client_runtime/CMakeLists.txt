add_library(dots_client_runtime STATIC src/client_runtime.cpp)
add_library(Dots::ClientRuntime ALIAS dots_client_runtime)

target_include_directories(
    dots_client_runtime
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_client_runtime
    PUBLIC
        Dots::Protocol
        Dots::Replication
        MyCore::Math
        MyCore::NetTransport
    PRIVATE
        MyCore::Debug
)
mycore_configure_target(dots_client_runtime)
