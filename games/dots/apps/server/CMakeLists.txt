add_executable(dots_server main.cpp)
target_link_libraries(
    dots_server
    PRIVATE
        Dots::Server
        MyCore::Debug
)
mycore_configure_executable(dots_server)
