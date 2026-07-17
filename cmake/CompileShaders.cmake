include(CMakeParseArguments)

function(mycore_compile_hlsl_shaders target_name)
    cmake_parse_arguments(SHADER "" "OUTPUT_DIRECTORY" "SOURCES" ${ARGN})
    if(NOT SHADER_OUTPUT_DIRECTORY)
        message(FATAL_ERROR "mycore_compile_hlsl_shaders requires OUTPUT_DIRECTORY")
    endif()
    if(NOT SHADER_SOURCES)
        message(FATAL_ERROR "mycore_compile_hlsl_shaders requires at least one source")
    endif()

    if(WIN32)
        set(shader_destination DXIL)
        set(shader_extension dxil)
        set(shader_entrypoint main)
        find_program(MYCORE_DXC_EXECUTABLE NAMES dxc REQUIRED)
    elseif(APPLE)
        set(shader_destination MSL)
        set(shader_extension msl)
        set(shader_entrypoint main0)
        find_program(MYCORE_GLSLANG_EXECUTABLE NAMES glslangValidator REQUIRED)
        find_program(MYCORE_SPIRV_CROSS_EXECUTABLE NAMES spirv-cross REQUIRED)
    elseif(UNIX)
        set(shader_destination SPIRV)
        set(shader_extension spv)
        set(shader_entrypoint main)
        find_program(MYCORE_GLSLANG_EXECUTABLE NAMES glslangValidator REQUIRED)
    else()
        message(FATAL_ERROR "No SDL_GPU shader format is configured for this platform")
    endif()

    set(compiled_shaders)
    foreach(shader_source IN LISTS SHADER_SOURCES)
        get_filename_component(shader_source "${shader_source}" ABSOLUTE)
        get_filename_component(shader_filename "${shader_source}" NAME)
        if(shader_filename MATCHES "\\.vert\\.hlsl$")
            set(shader_stage vertex)
            set(glslang_stage vert)
            set(dxc_profile vs_6_0)
        elseif(shader_filename MATCHES "\\.frag\\.hlsl$")
            set(shader_stage fragment)
            set(glslang_stage frag)
            set(dxc_profile ps_6_0)
        else()
            message(FATAL_ERROR "Shader stage must be encoded as .vert.hlsl or .frag.hlsl: ${shader_source}")
        endif()

        string(REGEX REPLACE "\\.hlsl$" ".${shader_extension}" output_filename "${shader_filename}")
        set(shader_output "${SHADER_OUTPUT_DIRECTORY}/${output_filename}")
        if(WIN32)
            add_custom_command(
                OUTPUT "${shader_output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIRECTORY}"
                COMMAND
                    "${MYCORE_DXC_EXECUTABLE}"
                    -T "${dxc_profile}"
                    -E main
                    -Fo "${shader_output}"
                    "${shader_source}"
                DEPENDS "${shader_source}"
                COMMENT "Compiling ${shader_filename} to ${shader_destination}"
                VERBATIM
            )
        elseif(APPLE)
            set(intermediate_directory
                "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target_name}.dir/intermediate"
            )
            set(intermediate_spirv "${intermediate_directory}/${shader_filename}.spv")
            add_custom_command(
                OUTPUT "${shader_output}"
                BYPRODUCTS "${intermediate_spirv}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIRECTORY}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${intermediate_directory}"
                COMMAND
                    "${MYCORE_GLSLANG_EXECUTABLE}"
                    -D
                    -V
                    --target-env vulkan1.0
                    -S "${glslang_stage}"
                    -e main
                    -o "${intermediate_spirv}"
                    "${shader_source}"
                COMMAND
                    "${MYCORE_SPIRV_CROSS_EXECUTABLE}"
                    "${intermediate_spirv}"
                    --msl
                    --entry main
                    --stage "${glslang_stage}"
                    --output "${shader_output}"
                DEPENDS "${shader_source}"
                COMMENT "Compiling ${shader_filename} to ${shader_destination}"
                VERBATIM
            )
        else()
            add_custom_command(
                OUTPUT "${shader_output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIRECTORY}"
                COMMAND
                    "${MYCORE_GLSLANG_EXECUTABLE}"
                    -D
                    -V
                    --target-env vulkan1.0
                    -S "${glslang_stage}"
                    -e main
                    -o "${shader_output}"
                    "${shader_source}"
                DEPENDS "${shader_source}"
                COMMENT "Compiling ${shader_filename} to ${shader_destination}"
                VERBATIM
            )
        endif()
        list(APPEND compiled_shaders "${shader_output}")
    endforeach()

    add_custom_target("${target_name}" DEPENDS ${compiled_shaders})
    set("${target_name}_OUTPUTS" "${compiled_shaders}" PARENT_SCOPE)
    set("${target_name}_FORMAT" "${shader_destination}" PARENT_SCOPE)
    set("${target_name}_EXTENSION" "${shader_extension}" PARENT_SCOPE)
    set("${target_name}_ENTRYPOINT" "${shader_entrypoint}" PARENT_SCOPE)
endfunction()
