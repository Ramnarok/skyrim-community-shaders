# SkyrimRT: compile the Shader Model 6 shaders (inline RayQuery needs cs_6_5) with the Windows SDK's DXC
# at build time. DXC signs the DXIL through the SDK's dxil.dll, so nothing extra ships to the game; the
# .cso files are installed into Shaders/SkyrimRT next to the FXC shaders CS compiles at runtime.
# Sources live in src/RT/Shaders so CS's FXC-based shader validation never sees them.

set(SKYRIMRT_SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/RT/Shaders")
set(SKYRIMRT_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/SkyrimRTShaders")

# Prefer the SDK the VS generator targets; otherwise (e.g. Ninja presets) take the newest installed SDK.
set(_skyrimrt_dxc_hints)
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    list(APPEND _skyrimrt_dxc_hints
        "$ENV{WindowsSdkDir}bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
endif()
file(GLOB _skyrimrt_sdk_dxcs "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxc.exe")
list(SORT _skyrimrt_sdk_dxcs COMPARE NATURAL ORDER DESCENDING)
foreach(_dxc IN LISTS _skyrimrt_sdk_dxcs)
    get_filename_component(_dxc_dir "${_dxc}" DIRECTORY)
    list(APPEND _skyrimrt_dxc_hints "${_dxc_dir}")
endforeach()

find_program(SKYRIMRT_DXC dxc HINTS ${_skyrimrt_dxc_hints} NO_DEFAULT_PATH)
if(NOT SKYRIMRT_DXC)
    message(FATAL_ERROR "SkyrimRT: dxc.exe not found in the Windows SDK (needed for cs_6_5 RayQuery shaders)")
endif()
message(STATUS "SkyrimRT: using DXC ${SKYRIMRT_DXC}")

file(GLOB SKYRIMRT_SHADER_SOURCES CONFIGURE_DEPENDS "${SKYRIMRT_SHADER_SOURCE_DIR}/*.hlsl")
file(GLOB SKYRIMRT_SHADER_INCLUDES CONFIGURE_DEPENDS "${SKYRIMRT_SHADER_SOURCE_DIR}/*.hlsli")
set(SKYRIMRT_SHADER_OUTPUTS)
foreach(_source IN LISTS SKYRIMRT_SHADER_SOURCES)
    get_filename_component(_name "${_source}" NAME_WE)
    set(_output "${SKYRIMRT_SHADER_OUTPUT_DIR}/${_name}.cso")
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SKYRIMRT_SHADER_OUTPUT_DIR}"
        COMMAND "${SKYRIMRT_DXC}" -nologo -T cs_6_5 -E main -O3 -I "${SKYRIMRT_SHADER_SOURCE_DIR}" -Fo "${_output}" "${_source}"
        DEPENDS "${_source}" ${SKYRIMRT_SHADER_INCLUDES}
        COMMENT "SkyrimRT: dxc ${_name}.hlsl"
        VERBATIM
    )
    list(APPEND SKYRIMRT_SHADER_OUTPUTS "${_output}")
endforeach()

add_custom_target(SkyrimRTShaders ALL DEPENDS ${SKYRIMRT_SHADER_OUTPUTS})
add_dependencies(${PROJECT_NAME} SkyrimRTShaders)

install(FILES ${SKYRIMRT_SHADER_OUTPUTS} DESTINATION Shaders/SkyrimRT COMPONENT Shaders)

# With the incremental AIO preparation (AUTO_PLUGIN_DEPLOYMENT / AIO_ZIP_TO_DIST) the AIO target copies files
# instead of installing, and its CleanupStaleEntries step deletes anything in AIO/Shaders it doesn't track.
# So copy the .cso files as a POST_BUILD step of the AIO target itself, after that cleanup. This file must be
# included after the AIO target is defined.
if(TARGET AIO)
    add_dependencies(AIO SkyrimRTShaders)
    add_custom_command(
        TARGET AIO
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/SkyrimRT"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SKYRIMRT_SHADER_OUTPUTS} "${AIO_DIR}/Shaders/SkyrimRT"
        COMMENT "SkyrimRT: copying DXC shaders into AIO/Shaders/SkyrimRT"
        VERBATIM
    )
endif()
