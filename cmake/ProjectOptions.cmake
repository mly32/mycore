include(CompilerWarnings)

function(mycore_create_project_options)
    option(
        MYCORE_ENABLE_SANITIZERS
        "Build project targets with AddressSanitizer and UndefinedBehaviorSanitizer"
        OFF
    )
    option(MYCORE_BUILD_FUZZERS "Build coverage-guided fuzz targets" OFF)

    if(MYCORE_ENABLE_SANITIZERS OR MYCORE_BUILD_FUZZERS)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "MyCore sanitizer and fuzzing builds require Clang on Linux")
        endif()
    endif()
    if(MYCORE_BUILD_FUZZERS AND NOT MYCORE_ENABLE_SANITIZERS)
        message(FATAL_ERROR "MYCORE_BUILD_FUZZERS requires MYCORE_ENABLE_SANITIZERS")
    endif()

    add_library(mycore_project_options INTERFACE)
    add_library(mycore::project_options ALIAS mycore_project_options)
    target_compile_features(mycore_project_options INTERFACE cxx_std_20)

    if(MYCORE_ENABLE_SANITIZERS)
        target_compile_options(
            mycore_project_options
            INTERFACE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )
        target_link_options(
            mycore_project_options
            INTERFACE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )
    endif()

    add_library(mycore_project_warnings INTERFACE)
    add_library(mycore::project_warnings ALIAS mycore_project_warnings)
    mycore_set_project_warnings(mycore_project_warnings)
endfunction()

function(mycore_enable_fuzz_coverage target_name)
    if(NOT MYCORE_BUILD_FUZZERS)
        message(FATAL_ERROR "Fuzz coverage requested without MYCORE_BUILD_FUZZERS")
    endif()
    target_compile_options("${target_name}" PRIVATE -fsanitize=fuzzer-no-link)
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

function(mycore_configure_fuzzer target_name)
    if(NOT MYCORE_BUILD_FUZZERS)
        message(FATAL_ERROR "Fuzzer target requested without MYCORE_BUILD_FUZZERS")
    endif()
    mycore_configure_executable("${target_name}")
    target_compile_options("${target_name}" PRIVATE -fsanitize=fuzzer)
    target_link_options("${target_name}" PRIVATE -fsanitize=fuzzer)
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
