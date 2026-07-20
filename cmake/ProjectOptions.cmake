include(CompilerWarnings)

function(mycore_create_project_options)
    add_library(mycore_project_options INTERFACE)
    add_library(mycore::project_options ALIAS mycore_project_options)
    target_compile_features(mycore_project_options INTERFACE cxx_std_20)

    add_library(mycore_project_warnings INTERFACE)
    add_library(mycore::project_warnings ALIAS mycore_project_warnings)
    mycore_set_project_warnings(mycore_project_warnings)
endfunction()

function(mycore_configure_target target_name)
    set_target_properties("${target_name}" PROPERTIES CXX_EXTENSIONS OFF)

    target_link_libraries(
        "${target_name}"
        PRIVATE
            mycore::project_options
            mycore::project_warnings
    )
endfunction()

function(mycore_configure_executable target_name)
    mycore_configure_target("${target_name}")
    set_target_properties(
        "${target_name}"
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

function(mycore_configure_test_executable target_name)
    mycore_configure_executable("${target_name}")
    mycore_suppress_test_warnings("${target_name}")
endfunction()

function(mycore_stage_runtime_dependencies target_name)
    if(WIN32)
        add_custom_command(
            TARGET "${target_name}"
            POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target_name}>
                $<TARGET_FILE_DIR:${target_name}>
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
endfunction()
