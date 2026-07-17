find_package(Catch2 3 CONFIG REQUIRED)

add_executable(mycore_core_smoke_tests mycore_core_smoke_tests.cpp)
target_link_libraries(mycore_core_smoke_tests PRIVATE MyCore::Core Catch2::Catch2WithMain)
mycore_configure_executable(mycore_core_smoke_tests)

include(Catch)
catch_discover_tests(mycore_core_smoke_tests)
