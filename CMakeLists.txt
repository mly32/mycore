cmake_minimum_required(VERSION 3.25)

# Modern Apple and Windows linkers can safely de-duplicate static libraries. Keep compatibility
# with the declared CMake minimum while opting into linker-aware behavior when it is available.
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()
if(POLICY CMP0179)
    cmake_policy(SET CMP0179 NEW)
endif()

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

include(Packaging)
