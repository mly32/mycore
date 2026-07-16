cmake_minimum_required(VERSION 3.25)

project(
    MyCore
    VERSION 0.1.0
    DESCRIPTION "A minimal C++ game engine and the Dots authoritative multiplayer game"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(ProjectOptions)

mycore_create_project_options()

add_subdirectory(engine)
add_subdirectory(games)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
