# SkyrimRT M6: NVIDIA Real-time Denoisers (NRD) for the ray-traced GI.
#
# NRD is licensed under the NVIDIA RTX SDKs License, whose section 4(e) forbids using the SDK "in any manner that would
# cause it to become subject to an open source software license". This project is GPL-3.0, so NRD is OFF by default:
# builds with SKYRIMRT_NRD=ON are for private use only and must never be distributed. Without it, ray-traced GI is
# compiled out and the feature reports it as unavailable.

option(SKYRIMRT_NRD "Build Skyrim RT's GI with NVIDIA NRD (NVIDIA RTX SDKs License: private builds only, never distribute)" OFF)
if(NOT SKYRIMRT_NRD)
    return()
endif()

set(SKYRIMRT_NRD_DIR "${CMAKE_SOURCE_DIR}/extern/RayTracingDenoiser")
if(NOT EXISTS "${SKYRIMRT_NRD_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SkyrimRT: SKYRIMRT_NRD=ON but ${SKYRIMRT_NRD_DIR} is missing (git submodule update --init extern/RayTracingDenoiser)")
endif()
message(WARNING "SkyrimRT: NRD enabled. This build contains NVIDIA RTX SDK code: private use only, do not distribute it.")

# Static library, DXIL only (the sidecar is D3D12), no NRI: the sidecar drives nrd::GetComputeDispatches itself.
set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRD_NRI OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_NORMAL_ENCODING "2" CACHE STRING "" FORCE)     # R10G10B10A2_UNORM, octahedral
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "" FORCE)  # linear roughness
set(NRD_SHADERS_PATH "${CMAKE_BINARY_DIR}/NRDShaders" CACHE STRING "" FORCE)
# ShaderMake (fetched by NRD) downloads its own DXC for DXIL; FXC (DXBC) and the Vulkan SDK's DXC (SPIR-V) aren't needed.
set(SHADERMAKE_FIND_FXC OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_DXC_VK OFF CACHE BOOL "" FORCE)

add_subdirectory("${SKYRIMRT_NRD_DIR}" "${CMAKE_BINARY_DIR}/NRD" EXCLUDE_FROM_ALL)

# NRD builds with /W4 /WX; a newer MSVC than NRD was tested with must not fail our build over a warning.
if(MSVC)
    target_compile_options(NRD PRIVATE /WX-)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE NRD)
target_compile_definitions(${PROJECT_NAME} PRIVATE SKYRIMRT_NRD=1)
