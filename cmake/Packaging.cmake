if(NOT TARGET dots_client OR NOT TARGET mycore_render_2d_shader_assets)
    message(FATAL_ERROR "Dots client packaging requires dots_client and Render2D shaders")
endif()

if(WIN32)
    set(mycore_package_system windows)
    set(mycore_package_generator ZIP)
    set(mycore_package_extension zip)
elseif(APPLE)
    set(mycore_package_system macos)
    set(mycore_package_generator TGZ)
    set(mycore_package_extension tar.gz)
else()
    set(mycore_package_system linux)
    set(mycore_package_generator TGZ)
    set(mycore_package_extension tar.gz)
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" mycore_package_architecture)
if(NOT mycore_package_architecture)
    set(mycore_package_architecture unknown)
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" mycore_package_configuration)
if(NOT mycore_package_configuration)
    set(mycore_package_configuration multi-config)
endif()

set(
    mycore_package_basename
    "dots-client-${PROJECT_VERSION}-${mycore_package_system}-${mycore_package_architecture}-${mycore_package_configuration}"
)
set(CPACK_GENERATOR "${mycore_package_generator}")
set(CPACK_PACKAGE_NAME "dots-client")
set(CPACK_PACKAGE_VENDOR "MyCore")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Playable offline Dots SDL_GPU client")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${mycore_package_basename}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_COMPONENTS_ALL DotsClient)
set(CPACK_PACKAGE_CHECKSUM SHA256)

include(CPack)

get_target_property(
    mycore_package_shader_extension
    mycore_render_2d
    MYCORE_RUNTIME_SHADER_EXTENSION
)
if(NOT mycore_package_shader_extension)
    message(FATAL_ERROR "MyCore::Render2D did not declare its shader extension")
endif()

set(
    mycore_package_file
    "${CPACK_PACKAGE_DIRECTORY}/${mycore_package_basename}.${mycore_package_extension}"
)
add_custom_target(
    dots_client_package
    COMMAND
        "${CMAKE_CPACK_COMMAND}"
        --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
        -C "${CMAKE_BUILD_TYPE}"
    COMMAND
        "${CMAKE_COMMAND}"
        "-DMYCORE_PACKAGE_FILE=${mycore_package_file}"
        "-DMYCORE_PACKAGE_SYSTEM=${mycore_package_system}"
        "-DMYCORE_SHADER_EXTENSION=${mycore_package_shader_extension}"
        "-DMYCORE_VERIFY_DIRECTORY=${CMAKE_BINARY_DIR}/package-verify"
        -P "${CMAKE_SOURCE_DIR}/cmake/VerifyDotsClientPackage.cmake"
    DEPENDS dots_client mycore_render_2d_shader_assets
    COMMENT "Packaging and verifying ${mycore_package_basename}"
    VERBATIM
)
