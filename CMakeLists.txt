cmake_minimum_required(VERSION 3.21)

if(NOT WIN32)
    message(FATAL_ERROR "Corona currently supports Windows builds only.")
endif()

set(CORONA_WINDOWS_SDK_VERSION "10.0.18362.0")

if(CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "${CORONA_WINDOWS_SDK_VERSION}" CACHE STRING "Windows SDK version" FORCE)
endif()

project(Corona LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(Vulkan QUIET)

set(CORONA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(CORONA_OUT_DIR "${CORONA_SRC_DIR}")

file(GLOB CORONA_HEADERS CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.h"
)

file(GLOB CORONA_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.cpp"
)

list(APPEND CORONA_SOURCES
    "${CORONA_SRC_DIR}/VulkanBackend.cpp"
)

file(GLOB CORONA_ENKITS_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/external/enkiTS/*.cpp"
)

file(GLOB CORONA_IMGUI_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/external/imgui/*.cpp"
)

set(CORONA_BACKEND_SOURCES
    "${CORONA_SRC_DIR}/external/imgui/backends/imgui_impl_dx12.cpp"
    "${CORONA_SRC_DIR}/external/imgui/backends/imgui_impl_win32.cpp"
)
if(Vulkan_FOUND)
    list(APPEND CORONA_BACKEND_SOURCES
        "${CORONA_SRC_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp"
    )
endif()

file(GLOB CORONA_WINPIX_PACKAGES LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/build/packages/WinPixEventRuntime.*"
)

list(LENGTH CORONA_WINPIX_PACKAGES CORONA_WINPIX_PACKAGE_COUNT)
if(CORONA_WINPIX_PACKAGE_COUNT EQUAL 0)
    message(FATAL_ERROR "WinPixEventRuntime package not found under build/packages. Restore the package before configuring with CMake.")
endif()

list(SORT CORONA_WINPIX_PACKAGES COMPARE NATURAL ORDER DESCENDING)
list(GET CORONA_WINPIX_PACKAGES 0 CORONA_WINPIX_ROOT)

set(CORONA_DIRECTXTEX_LIB_DIR "${CORONA_SRC_DIR}/external/DirectXTex July 2017/Lib 2017")
set(CORONA_ASSIMP_LIB "${CORONA_SRC_DIR}/external/assimp/lib/assimp.lib")
set(CORONA_DXCOMPILER_LIB "${CORONA_SRC_DIR}/external/dxc/lib/x64/dxcompiler.lib")
set(CORONA_AFTERMATH_LIB "${CORONA_SRC_DIR}/external/GFSDK_Aftermath/lib/x64/GFSDK_Aftermath_Lib.x64.lib")
set(CORONA_AFTERMATH_DLL "${CORONA_SRC_DIR}/external/GFSDK_Aftermath/lib/x64/GFSDK_Aftermath_Lib.x64.dll")
set(CORONA_WINPIX_LIB "${CORONA_WINPIX_ROOT}/bin/x64/WinPixEventRuntime.lib")
set(CORONA_WINPIX_DLL "${CORONA_WINPIX_ROOT}/bin/x64/WinPixEventRuntime.dll")
set(CORONA_STREAMLINE_SDK_ROOT "H:/dev/streamline_test/streamline-sdk" CACHE PATH "Path to the local NVIDIA Streamline SDK")
set(CORONA_STREAMLINE_LIB "")
if(EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/include/sl.h")
    set(CORONA_STREAMLINE_LIB "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib")
endif()

foreach(required_path IN ITEMS
    "${CORONA_ASSIMP_LIB}"
    "${CORONA_DXCOMPILER_LIB}"
    "${CORONA_AFTERMATH_LIB}"
    "${CORONA_WINPIX_LIB}"
)
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Required dependency was not found: ${required_path}")
    endif()
endforeach()

add_executable(Corona WIN32
    ${CORONA_HEADERS}
    ${CORONA_SOURCES}
    ${CORONA_ENKITS_SOURCES}
    ${CORONA_IMGUI_SOURCES}
    ${CORONA_BACKEND_SOURCES}
)

target_include_directories(Corona PRIVATE
    "${CORONA_SRC_DIR}/external"
    "${CORONA_SRC_DIR}/external/DirectXTex July 2017/Include"
    "${CORONA_SRC_DIR}/external/assimp/include"
    "${CORONA_SRC_DIR}/external/imgui"
    "${CORONA_SRC_DIR}/external/imgui/backends"
    "${CORONA_SRC_DIR}/external/dxc/inc"
    "${CORONA_WINPIX_ROOT}/Include/WinPixEventRuntime"
    "${CORONA_SRC_DIR}"
)
if(Vulkan_FOUND)
    target_include_directories(Corona PRIVATE ${Vulkan_INCLUDE_DIRS})
endif()
if(CORONA_STREAMLINE_LIB)
    target_include_directories(Corona PRIVATE
        "${CORONA_STREAMLINE_SDK_ROOT}/include"
    )
endif()

target_link_directories(Corona PRIVATE
    "${CORONA_DIRECTXTEX_LIB_DIR}"
)

target_link_libraries(Corona PRIVATE
    d3dcompiler
    dxguid
    "${CORONA_ASSIMP_LIB}"
    "${CORONA_DXCOMPILER_LIB}"
    "${CORONA_AFTERMATH_LIB}"
    "${CORONA_WINPIX_LIB}"
)
if(CORONA_STREAMLINE_LIB)
    target_link_libraries(Corona PRIVATE "${CORONA_STREAMLINE_LIB}")
else()
    target_link_libraries(Corona PRIVATE d3d12 dxgi)
endif()
if(Vulkan_FOUND)
    target_link_libraries(Corona PRIVATE Vulkan::Vulkan)
endif()

if(Vulkan_FOUND)
    set(CORONA_VULKAN_SHADER_DIR "${CORONA_SRC_DIR}/Shaders")
    set(CORONA_VULKAN_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/VulkanGeneratedShaders")
    set(CORONA_VULKAN_SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/VulkanShaders")
    set(CORONA_VULKAN_RT_SHADER_PREPARE "${CORONA_VULKAN_SHADER_DIR}/PrepareVulkanRTShader.ps1")
    set(CORONA_VULKAN_RT_SHADOW_HLSL "${CORONA_VULKAN_GENERATED_SHADER_DIR}/RaytracedShadowVulkan.generated.hlsl")
    set(CORONA_VULKAN_RT_REFLECTION_HLSL "${CORONA_VULKAN_GENERATED_SHADER_DIR}/RaytracedReflectionVulkan.generated.hlsl")
    set(CORONA_VULKAN_RT_GI_HLSL "${CORONA_VULKAN_GENERATED_SHADER_DIR}/RaytracedGIVulkan.generated.hlsl")
    set(CORONA_VULKAN_RT_PATH_TRACING_HLSL "${CORONA_VULKAN_GENERATED_SHADER_DIR}/PathTracingVulkan.generated.hlsl")
    set(CORONA_VULKAN_RT_GENERATED_SHADERS
        "${CORONA_VULKAN_RT_SHADOW_HLSL}"
        "${CORONA_VULKAN_RT_REFLECTION_HLSL}"
        "${CORONA_VULKAN_RT_GI_HLSL}"
        "${CORONA_VULKAN_RT_PATH_TRACING_HLSL}"
    )
    set(CORONA_VULKAN_SHADER_OUTPUTS
        "${CORONA_VULKAN_SPIRV_DIR}/GBufferVulkan.vert.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/GBufferVulkan.frag.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/LightingPSVulkan.vert.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/LightingPSVulkan.frag.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/ToneMapPSVulkan.vert.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/ToneMapPSVulkan.frag.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/TemporalAAVulkan.vert.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/TemporalAAVulkan.frag.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/ShadowDenoiseVulkan.comp.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/TemporalDenoisingVulkan.comp.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/SpatialDenoisingVulkan.comp.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/RaytracedShadowVulkan.rt.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/RaytracedReflectionVulkan.rt.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/RaytracedGIVulkan.rt.spv"
        "${CORONA_VULKAN_SPIRV_DIR}/PathTracingVulkan.rt.spv"
    )

    find_program(
        CORONA_VULKAN_DXC
        NAMES dxc.exe
        PATHS
            "$ENV{VULKAN_SDK}/Bin"
            "C:/VulkanSDK/1.4.341.1/Bin"
        NO_DEFAULT_PATH
    )

    if(CORONA_VULKAN_DXC)
        message(STATUS "Using DXC for Vulkan HLSL shaders: ${CORONA_VULKAN_DXC}")
        set(CORONA_VULKAN_DXC_COMMON_FLAGS
            -O3
            -spirv
            -DVULKAN_SPIRV=1
            -fvk-use-dx-layout
            -fvk-t-shift 0 all
            -fvk-u-shift 32 all
            -fvk-s-shift 64 all
            -fvk-b-shift 128 all
            -I "${CORONA_VULKAN_SHADER_DIR}"
        )
        add_custom_command(
            OUTPUT ${CORONA_VULKAN_SHADER_OUTPUTS}
            BYPRODUCTS ${CORONA_VULKAN_RT_GENERATED_SHADERS}
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CORONA_VULKAN_GENERATED_SHADER_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CORONA_VULKAN_SPIRV_DIR}"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T vs_6_0 -E VSMain "${CORONA_VULKAN_SHADER_DIR}/GBuffer.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/GBufferVulkan.vert.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T ps_6_0 -E PSMain "${CORONA_VULKAN_SHADER_DIR}/GBuffer.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/GBufferVulkan.frag.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T vs_6_0 -E VSMain "${CORONA_VULKAN_SHADER_DIR}/LightingPS.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/LightingPSVulkan.vert.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T ps_6_0 -E PSMain "${CORONA_VULKAN_SHADER_DIR}/LightingPS.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/LightingPSVulkan.frag.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T vs_6_0 -E VSMain "${CORONA_VULKAN_SHADER_DIR}/ToneMapPS.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/ToneMapPSVulkan.vert.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T ps_6_0 -E PSMain "${CORONA_VULKAN_SHADER_DIR}/ToneMapPS.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/ToneMapPSVulkan.frag.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T vs_6_0 -E VSMain "${CORONA_VULKAN_SHADER_DIR}/TemporalAA.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/TemporalAAVulkan.vert.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T ps_6_0 -E PSMain "${CORONA_VULKAN_SHADER_DIR}/TemporalAA.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/TemporalAAVulkan.frag.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T cs_6_0 -E ShadowDenoiseCS "${CORONA_VULKAN_SHADER_DIR}/ShadowDenoise.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/ShadowDenoiseVulkan.comp.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T cs_6_0 -E TemporalFilter "${CORONA_VULKAN_SHADER_DIR}/TemporalDenoising.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/TemporalDenoisingVulkan.comp.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T cs_6_0 -E SpatialFilter "${CORONA_VULKAN_SHADER_DIR}/SpatialDenoising.hlsl" -Fo "${CORONA_VULKAN_SPIRV_DIR}/SpatialDenoisingVulkan.comp.spv"
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CORONA_VULKAN_RT_SHADER_PREPARE}" -InputPath "${CORONA_VULKAN_SHADER_DIR}/RaytracedShadow.hlsl" -OutputPath "${CORONA_VULKAN_RT_SHADOW_HLSL}"
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CORONA_VULKAN_RT_SHADER_PREPARE}" -InputPath "${CORONA_VULKAN_SHADER_DIR}/RaytracedReflection.hlsl" -OutputPath "${CORONA_VULKAN_RT_REFLECTION_HLSL}"
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CORONA_VULKAN_RT_SHADER_PREPARE}" -InputPath "${CORONA_VULKAN_SHADER_DIR}/RaytracedGI.hlsl" -OutputPath "${CORONA_VULKAN_RT_GI_HLSL}"
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CORONA_VULKAN_RT_SHADER_PREPARE}" -InputPath "${CORONA_VULKAN_SHADER_DIR}/PathTracing.hlsl" -OutputPath "${CORONA_VULKAN_RT_PATH_TRACING_HLSL}"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.2 -T lib_6_3 "${CORONA_VULKAN_RT_SHADOW_HLSL}" -Fo "${CORONA_VULKAN_SPIRV_DIR}/RaytracedShadowVulkan.rt.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.2 -T lib_6_3 "${CORONA_VULKAN_RT_REFLECTION_HLSL}" -Fo "${CORONA_VULKAN_SPIRV_DIR}/RaytracedReflectionVulkan.rt.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.2 -T lib_6_3 "${CORONA_VULKAN_RT_GI_HLSL}" -Fo "${CORONA_VULKAN_SPIRV_DIR}/RaytracedGIVulkan.rt.spv"
            COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.2 -T lib_6_3 "${CORONA_VULKAN_RT_PATH_TRACING_HLSL}" -Fo "${CORONA_VULKAN_SPIRV_DIR}/PathTracingVulkan.rt.spv"
            DEPENDS "${CORONA_VULKAN_SHADER_DIR}/GBuffer.hlsl" "${CORONA_VULKAN_SHADER_DIR}/LightingPS.hlsl" "${CORONA_VULKAN_SHADER_DIR}/ToneMapPS.hlsl" "${CORONA_VULKAN_SHADER_DIR}/TemporalAA.hlsl" "${CORONA_VULKAN_SHADER_DIR}/ShadowDenoise.hlsl" "${CORONA_VULKAN_SHADER_DIR}/TemporalDenoising.hlsl" "${CORONA_VULKAN_SHADER_DIR}/SpatialDenoising.hlsl" "${CORONA_VULKAN_SHADER_DIR}/RaytracedShadow.hlsl" "${CORONA_VULKAN_SHADER_DIR}/RaytracedReflection.hlsl" "${CORONA_VULKAN_SHADER_DIR}/RaytracedGI.hlsl" "${CORONA_VULKAN_SHADER_DIR}/PathTracing.hlsl" "${CORONA_VULKAN_SHADER_DIR}/Common.hlsl" "${CORONA_VULKAN_SHADER_DIR}/ShaderResourceBindings.hlsli" "${CORONA_VULKAN_RT_SHADER_PREPARE}"
            VERBATIM
        )
    endif()

    if(CORONA_VULKAN_DXC)
        add_custom_target(CoronaVulkanShaders DEPENDS ${CORONA_VULKAN_SHADER_OUTPUTS})
        add_dependencies(Corona CoronaVulkanShaders)
    endif()
    target_compile_definitions(Corona PRIVATE CORONA_VULKAN_SHADER_BINARY_DIR=L"${CORONA_VULKAN_SPIRV_DIR}")
endif()

target_compile_definitions(Corona PRIVATE
    IMGUI_DEFINE_MATH_OPERATORS
    $<$<BOOL:${Vulkan_FOUND}>:CORONA_HAS_VULKAN=1>
    $<$<NOT:$<BOOL:${Vulkan_FOUND}>>:CORONA_HAS_VULKAN=0>
    $<$<BOOL:${CORONA_STREAMLINE_LIB}>:WITH_STREAMLINE=1>
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

set_target_properties(Corona PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CORONA_SRC_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CORONA_OUT_DIR}"
)

add_custom_command(TARGET Corona POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CORONA_AFTERMATH_DLL}"
        "${CORONA_OUT_DIR}/GFSDK_Aftermath_Lib.x64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CORONA_WINPIX_DLL}"
        "${CORONA_OUT_DIR}/WinPixEventRuntime.dll"
)
if(CORONA_STREAMLINE_LIB)
    foreach(CORONA_STREAMLINE_DLL IN ITEMS sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll sl.dlss_d.dll nvngx_dlssd.dll)
        if(EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/bin/x64/${CORONA_STREAMLINE_DLL}")
            add_custom_command(TARGET Corona POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CORONA_STREAMLINE_SDK_ROOT}/bin/x64/${CORONA_STREAMLINE_DLL}"
                    "${CORONA_OUT_DIR}/${CORONA_STREAMLINE_DLL}"
            )
        endif()
    endforeach()
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Corona)
