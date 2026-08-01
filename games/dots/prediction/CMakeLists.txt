add_library(
    dots_prediction STATIC
    src/model.cpp
    src/scope.cpp
)
add_library(Dots::Prediction ALIAS dots_prediction)

target_include_directories(
    dots_prediction
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    dots_prediction
    PUBLIC
        Dots::Simulation
        MyCore::Rollback
)
mycore_configure_target(dots_prediction)
