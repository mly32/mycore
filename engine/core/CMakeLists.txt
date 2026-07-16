add_library(mycore_core STATIC src/core.cpp)
add_library(MyCore::Core ALIAS mycore_core)

target_include_directories(
    mycore_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

mycore_configure_target(mycore_core)
