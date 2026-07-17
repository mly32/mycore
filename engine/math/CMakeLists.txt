add_library(mycore_math INTERFACE)
add_library(MyCore::Math ALIAS mycore_math)

target_include_directories(
    mycore_math
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    mycore_math
    INTERFACE
        mycore::project_options
        mycore::project_warnings
)
