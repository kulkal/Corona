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
set(CORONA_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")

file(GLOB CORONA_HEADERS CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.h"
)

file(GLOB CORONA_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.cpp"
)

file(GLOB CORONA_SHADER_FILES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/Shaders/*.hlsl"
    "${CORONA_SRC_DIR}/Shaders/*.hlsli"
    "${CORONA_SRC_DIR}/Shaders/*.ps1"
)
set_source_files_properties(${CORONA_SHADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group(TREE "${CORONA_SRC_DIR}/Shaders" PREFIX "Shaders" FILES ${CORONA_SHADER_FILES})

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
option(CORONA_USE_AFTERMATH "Enable NVIDIA Aftermath markers for D3D12 GPU crash diagnostics" ON)
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
    ${CORONA_SHADER_FILES}
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
    # Dear ImGui's stock DX12 backend still compiles its embedded UI shaders with D3DCompile.
    # Corona runtime shaders use DXC.
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

add_executable(CoronaTextureImport
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/TextureImportTool.cpp"
)
target_include_directories(CoronaTextureImport PRIVATE
    "${CORONA_SRC_DIR}/external/DirectXTex July 2017/Include"
)
target_link_libraries(CoronaTextureImport PRIVATE
    "$<$<CONFIG:Debug>:${CORONA_DIRECTXTEX_LIB_DIR}/Debug/DirectXTex.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${CORONA_DIRECTXTEX_LIB_DIR}/Release/DirectXTex.lib>"
    dxguid
    windowscodecs
)
set_target_properties(CoronaTextureImport PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CORONA_OUT_DIR}"
)

if(Vulkan_FOUND)
    set(CORONA_VULKAN_SHADER_DIR "${CORONA_SRC_DIR}/Shaders")
    set(CORONA_VULKAN_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/VulkanGeneratedShaders")
    set(CORONA_VULKAN_SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/VulkanShaders")
    set(CORONA_VULKAN_RT_SHADER_PREPARE "${CORONA_VULKAN_SHADER_DIR}/PrepareVulkanRTShader.ps1")

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

        set(CORONA_VULKAN_SHADER_OUTPUTS)
        set(CORONA_VULKAN_RT_GENERATED_SHADERS)
        file(GLOB CORONA_VULKAN_HLSL_FILES CONFIGURE_DEPENDS
            "${CORONA_VULKAN_SHADER_DIR}/*.hlsl"
        )

        macro(CORONA_ADD_VULKAN_GRAPHICS_SHADER shader_path shader_stem)
            set(vertex_output "${CORONA_VULKAN_SPIRV_DIR}/${shader_stem}Vulkan.vert.spv")
            set(fragment_output "${CORONA_VULKAN_SPIRV_DIR}/${shader_stem}Vulkan.frag.spv")
            list(APPEND CORONA_VULKAN_SHADER_OUTPUTS "${vertex_output}" "${fragment_output}")
            add_custom_command(
                OUTPUT "${vertex_output}" "${fragment_output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CORONA_VULKAN_SPIRV_DIR}"
                COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T vs_6_0 -E VSMain "${shader_path}" -Fo "${vertex_output}"
                COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T ps_6_0 -E PSMain "${shader_path}" -Fo "${fragment_output}"
                DEPENDS "${shader_path}" ${CORONA_SHADER_FILES}
                VERBATIM
            )
        endmacro()

        macro(CORONA_ADD_VULKAN_COMPUTE_SHADER shader_path shader_stem entry_point)
            set(compute_output "${CORONA_VULKAN_SPIRV_DIR}/${shader_stem}_${entry_point}Vulkan.comp.spv")
            list(APPEND CORONA_VULKAN_SHADER_OUTPUTS "${compute_output}")
            add_custom_command(
                OUTPUT "${compute_output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CORONA_VULKAN_SPIRV_DIR}"
                COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T cs_6_0 -E "${entry_point}" "${shader_path}" -Fo "${compute_output}"
                DEPENDS "${shader_path}" ${CORONA_SHADER_FILES}
                VERBATIM
            )
        endmacro()

        macro(CORONA_ADD_VULKAN_RT_SHADER shader_path shader_stem)
            set(generated_shader "${CORONA_VULKAN_GENERATED_SHADER_DIR}/${shader_stem}Vulkan.generated.hlsl")
            set(rt_output "${CORONA_VULKAN_SPIRV_DIR}/${shader_stem}Vulkan.rt.spv")
            list(APPEND CORONA_VULKAN_RT_GENERATED_SHADERS "${generated_shader}")
            list(APPEND CORONA_VULKAN_SHADER_OUTPUTS "${rt_output}")
            add_custom_command(
                OUTPUT "${rt_output}"
                BYPRODUCTS "${generated_shader}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CORONA_VULKAN_GENERATED_SHADER_DIR}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CORONA_VULKAN_SPIRV_DIR}"
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CORONA_VULKAN_RT_SHADER_PREPARE}" -InputPath "${shader_path}" -OutputPath "${generated_shader}"
                COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.2 -T lib_6_3 "${generated_shader}" -Fo "${rt_output}"
                DEPENDS "${shader_path}" "${CORONA_VULKAN_RT_SHADER_PREPARE}" ${CORONA_SHADER_FILES}
                VERBATIM
            )
        endmacro()

        foreach(CORONA_VULKAN_SHADER IN LISTS CORONA_VULKAN_HLSL_FILES)
            get_filename_component(CORONA_VULKAN_SHADER_STEM "${CORONA_VULKAN_SHADER}" NAME_WE)
            file(READ "${CORONA_VULKAN_SHADER}" CORONA_VULKAN_SHADER_CONTENT)

            if(CORONA_VULKAN_SHADER_CONTENT MATCHES [=[\[shader\("raygeneration"\)\]]=])
                CORONA_ADD_VULKAN_RT_SHADER("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}")
            endif()

            if(CORONA_VULKAN_SHADER_CONTENT MATCHES "VSMain[ \t\r\n]*\\(" AND CORONA_VULKAN_SHADER_CONTENT MATCHES "PSMain[ \t\r\n]*\\(")
                CORONA_ADD_VULKAN_GRAPHICS_SHADER("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}")
            endif()

            string(REGEX MATCHALL "\\[numthreads\\([^]]+\\)\\][ \t\r\n]*void[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*\\(" CORONA_VULKAN_COMPUTE_ENTRY_MATCHES "${CORONA_VULKAN_SHADER_CONTENT}")
            foreach(CORONA_VULKAN_COMPUTE_ENTRY_MATCH IN LISTS CORONA_VULKAN_COMPUTE_ENTRY_MATCHES)
                string(REGEX REPLACE "\\[numthreads\\([^]]+\\)\\][ \t\r\n]*void[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*\\(" "\\1" CORONA_VULKAN_COMPUTE_ENTRY "${CORONA_VULKAN_COMPUTE_ENTRY_MATCH}")
                CORONA_ADD_VULKAN_COMPUTE_SHADER("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "${CORONA_VULKAN_COMPUTE_ENTRY}")
            endforeach()
        endforeach()

        list(REMOVE_DUPLICATES CORONA_VULKAN_SHADER_OUTPUTS)
        list(LENGTH CORONA_VULKAN_SHADER_OUTPUTS CORONA_VULKAN_SHADER_OUTPUT_COUNT)
        message(STATUS "Discovered ${CORONA_VULKAN_SHADER_OUTPUT_COUNT} Vulkan shader outputs from ${CORONA_VULKAN_SHADER_DIR}")

        if(CORONA_VULKAN_SHADER_OUTPUTS)
            add_custom_target(CoronaVulkanShaders DEPENDS ${CORONA_VULKAN_SHADER_OUTPUTS})
            add_dependencies(Corona CoronaVulkanShaders)
        endif()
    endif()
    target_compile_definitions(Corona PRIVATE CORONA_VULKAN_SHADER_BINARY_DIR=L"${CORONA_VULKAN_SPIRV_DIR}")
endif()

target_compile_definitions(Corona PRIVATE
    IMGUI_DEFINE_MATH_OPERATORS
    $<$<BOOL:${Vulkan_FOUND}>:CORONA_HAS_VULKAN=1>
    $<$<NOT:$<BOOL:${Vulkan_FOUND}>>:CORONA_HAS_VULKAN=0>
    $<$<BOOL:${CORONA_STREAMLINE_LIB}>:WITH_STREAMLINE=1>
    USE_AFTERMATH=$<BOOL:${CORONA_USE_AFTERMATH}>
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

set_target_properties(Corona PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CORONA_OUT_DIR}"
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
