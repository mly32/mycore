add_library(dots_remote_presentation STATIC src/remote_presentation.cpp)
add_library(Dots::RemotePresentation ALIAS dots_remote_presentation)

target_include_directories(
    dots_remote_presentation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_remote_presentation
    PUBLIC
        Dots::Protocol
        Dots::Simulation
        MyCore::Math
)
mycore_configure_target(dots_remote_presentation)

add_library(
    dots_presentation STATIC
    src/presentation.cpp
    src/spectator_camera.cpp
)
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
        Dots::RemotePresentation
        Dots::Simulation
        MyCore::Render2D
)
mycore_configure_target(dots_presentation)

add_library(
    dots_rollback_presentation STATIC
    src/rollback_consequences.cpp
)
add_library(Dots::RollbackPresentation ALIAS dots_rollback_presentation)

target_include_directories(
    dots_rollback_presentation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_rollback_presentation
    PUBLIC
        Dots::Prediction
        Dots::Presentation
        MyCore::Rollback
)
mycore_configure_target(dots_rollback_presentation)
