add_library(dots_protocol STATIC src/codec.cpp)
add_library(Dots::Protocol ALIAS dots_protocol)

target_include_directories(
    dots_protocol
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(dots_protocol PUBLIC MyCore::Core)
mycore_configure_target(dots_protocol)
