add_library(mycore_assets STATIC src/directory_source.cpp)
add_library(MyCore::Assets ALIAS mycore_assets)

target_include_directories(
    mycore_assets
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(mycore_assets PUBLIC MyCore::Core)
mycore_configure_target(mycore_assets)
