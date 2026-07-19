find_package(GameNetworkingSockets CONFIG REQUIRED)

add_library(
    mycore_net_transport STATIC
    src/gamenetworkingsockets_network.cpp
    src/in_memory_network.cpp
)
add_library(MyCore::NetTransport ALIAS mycore_net_transport)

target_include_directories(
    mycore_net_transport
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    mycore_net_transport
    PUBLIC MyCore::Core
    PRIVATE GameNetworkingSockets::GameNetworkingSockets
)
mycore_configure_target(mycore_net_transport)
