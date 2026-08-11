find_package(GameNetworkingSockets CONFIG REQUIRED)

if(TARGET GameNetworkingSockets::GameNetworkingSockets)
    set(mycore_gamenetworkingsockets_target GameNetworkingSockets::GameNetworkingSockets)
elseif(TARGET GameNetworkingSockets::static)
    set(mycore_gamenetworkingsockets_target GameNetworkingSockets::static)
else()
    message(FATAL_ERROR "GameNetworkingSockets did not provide a supported CMake target")
endif()

add_library(
    mycore_net_transport STATIC
    src/gamenetworkingsockets_network.cpp
    src/in_memory_network.cpp
)
add_library(MyCore::NetTransport ALIAS mycore_net_transport)

if(MYCORE_ENABLE_SANITIZERS)
    # The packaged GameNetworkingSockets library does not expose the RTTI metadata that
    # Clang's vptr sanitizer needs for its polymorphic C++ interfaces. Keep ASan and every
    # other UBSan check enabled at this third-party boundary.
    set_source_files_properties(
        src/gamenetworkingsockets_network.cpp
        PROPERTIES COMPILE_OPTIONS -fno-sanitize=vptr
    )
endif()

target_include_directories(
    mycore_net_transport
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    mycore_net_transport
    PUBLIC MyCore::Core
    PRIVATE ${mycore_gamenetworkingsockets_target}
)
mycore_configure_target(mycore_net_transport)
