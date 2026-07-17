find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)

add_library(
    mycore_debug STATIC
    src/log.cpp
    src/metrics.cpp
)
add_library(MyCore::Debug ALIAS mycore_debug)

target_include_directories(
    mycore_debug
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    mycore_debug
    PUBLIC
        MyCore::Core
        fmt::fmt
        Tracy::TracyClient
    PRIVATE
        spdlog::spdlog
)
mycore_configure_target(mycore_debug)
