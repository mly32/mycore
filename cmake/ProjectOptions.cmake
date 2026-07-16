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
