foreach(required_variable IN ITEMS
        MYCORE_PACKAGE_FILE
        MYCORE_PACKAGE_SYSTEM
        MYCORE_SHADER_EXTENSION
        MYCORE_VERIFY_DIRECTORY
)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Package verification requires ${required_variable}")
    endif()
endforeach()

if(NOT EXISTS "${MYCORE_PACKAGE_FILE}")
    message(FATAL_ERROR "Package does not exist: ${MYCORE_PACKAGE_FILE}")
endif()

file(REMOVE_RECURSE "${MYCORE_VERIFY_DIRECTORY}")
file(MAKE_DIRECTORY "${MYCORE_VERIFY_DIRECTORY}")
file(
    ARCHIVE_EXTRACT
    INPUT "${MYCORE_PACKAGE_FILE}"
    DESTINATION "${MYCORE_VERIFY_DIRECTORY}"
)

if(MYCORE_PACKAGE_SYSTEM STREQUAL "macos")
    set(
        package_executable
        "${MYCORE_VERIFY_DIRECTORY}/Dots.app/Contents/MacOS/dots_client"
    )
    set(package_resource_root "${MYCORE_VERIFY_DIRECTORY}/Dots.app/Contents/Resources")
    if(NOT EXISTS "${MYCORE_VERIFY_DIRECTORY}/Dots.app/Contents/Info.plist")
        message(FATAL_ERROR "Packaged macOS application is missing Contents/Info.plist")
    endif()
elseif(MYCORE_PACKAGE_SYSTEM STREQUAL "windows")
    set(package_executable "${MYCORE_VERIFY_DIRECTORY}/dots_client.exe")
    set(package_resource_root "${MYCORE_VERIFY_DIRECTORY}")
else()
    set(package_executable "${MYCORE_VERIFY_DIRECTORY}/dots_client")
    set(package_resource_root "${MYCORE_VERIFY_DIRECTORY}")
endif()

if(NOT EXISTS "${package_executable}")
    message(FATAL_ERROR "Packaged executable is missing: ${package_executable}")
endif()

foreach(config_file IN ITEMS dots-client.example.toml dots-client.schema.json)
    if(NOT EXISTS "${package_resource_root}/${config_file}")
        message(FATAL_ERROR "Packaged configuration resource is missing: ${config_file}")
    endif()
endforeach()

set(package_shader_directory "${package_resource_root}/assets/mycore/render_2d/shaders")
set(
    expected_shader_names
    circle.vert
    circle.frag
    grid.vert
    grid.frag
)
foreach(shader_name IN LISTS expected_shader_names)
    set(shader_path "${package_shader_directory}/${shader_name}.${MYCORE_SHADER_EXTENSION}")
    if(NOT EXISTS "${shader_path}")
        message(FATAL_ERROR "Packaged shader is missing: ${shader_path}")
    endif()
    file(SIZE "${shader_path}" shader_size)
    if(shader_size EQUAL 0)
        message(FATAL_ERROR "Packaged shader is empty: ${shader_path}")
    endif()
endforeach()

file(GLOB packaged_shaders LIST_DIRECTORIES false "${package_shader_directory}/*")
list(LENGTH packaged_shaders packaged_shader_count)
if(NOT packaged_shader_count EQUAL 4)
    message(FATAL_ERROR "Expected exactly four packaged shaders, found ${packaged_shader_count}")
endif()

execute_process(
    COMMAND "${package_executable}" --help
    WORKING_DIRECTORY "${MYCORE_VERIFY_DIRECTORY}"
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_error
)
if(NOT help_result EQUAL 0 OR NOT help_output MATCHES "playable offline SDL_GPU client")
    message(
        FATAL_ERROR
        "Packaged client help failed (${help_result}): ${help_output}${help_error}"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "SDL_VIDEODRIVER=dummy"
        "${package_executable}" --package-smoke
    WORKING_DIRECTORY "${MYCORE_VERIFY_DIRECTORY}"
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_output
    ERROR_VARIABLE smoke_error
)
if(NOT smoke_result EQUAL 0)
    message(
        FATAL_ERROR
        "Packaged client smoke failed (${smoke_result}): ${smoke_output}${smoke_error}"
    )
endif()

message(STATUS "Verified Dots client package: ${MYCORE_PACKAGE_FILE}")
