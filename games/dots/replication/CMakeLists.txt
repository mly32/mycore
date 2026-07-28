add_library(dots_replication STATIC src/replication.cpp)
add_library(Dots::Replication ALIAS dots_replication)

target_include_directories(
    dots_replication
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_replication
    PUBLIC
        Dots::Protocol
        Dots::Simulation
    PRIVATE
        Dots::Prediction
)
mycore_configure_target(dots_replication)
