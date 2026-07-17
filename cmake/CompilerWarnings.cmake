function(mycore_set_project_warnings target_name)
    if(MSVC)
        target_compile_options(
            "${target_name}"
            INTERFACE
                /W4
                /permissive-
                /w14242
                /w14254
                /w14263
                /w14265
                /w14287
                /w14296
                /w14311
                /w14545
                /w14546
                /w14547
                /w14549
                /w14555
                /w14619
                /w14640
                /w14826
                /w14905
                /w14906
                /w14928
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            "${target_name}"
            INTERFACE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
                -Wsign-conversion
        )
    endif()
endfunction()

function(mycore_suppress_test_warnings target_name)
    # Suppress warnings from third-party test frameworks (e.g., Catch2)
    # Use an interface library to ensure flags come after project_warnings
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_library(${target_name}_test_suppressions INTERFACE)
        target_compile_options(
            ${target_name}_test_suppressions
            INTERFACE
                -Wno-c2y-extensions                          # Catch2 uses __COUNTER__ macro
                -Wno-missing-designated-field-initializers   # Tests often use partial initializers
        )
        target_link_libraries(${target_name} PRIVATE ${target_name}_test_suppressions)
    endif()
endfunction()
