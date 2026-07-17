add_library(mycore_time STATIC src/time.cpp)
add_library(MyCore::Time ALIAS mycore_time)

target_include_directories(
    mycore_time
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

mycore_configure_target(mycore_time)
