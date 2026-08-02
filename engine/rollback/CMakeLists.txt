add_library(mycore_rollback STATIC src/rollback.cpp)
add_library(MyCore::Rollback ALIAS mycore_rollback)

target_include_directories(
    mycore_rollback
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    mycore_rollback
    PUBLIC
        MyCore::Core
        MyCore::Time
)

mycore_configure_target(mycore_rollback)

if(MYCORE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
