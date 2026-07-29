add_executable(dots_bot main.cpp)
target_link_libraries(
    dots_bot
    PRIVATE
        Dots::AppCli
        Dots::ClientRuntime
        MyCore::Debug
        MyCore::NetTransport
        MyCore::Time
)
mycore_configure_executable(dots_bot)
mycore_stage_runtime_dependencies(dots_bot)
