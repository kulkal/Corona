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

file(GLOB_RECURSE CORONA_SCRIPT_FILES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/scripts/*.luau"
)
set_source_files_properties(${CORONA_SCRIPT_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group(TREE "${CORONA_SRC_DIR}/scripts" PREFIX "Scripts" FILES ${CORONA_SCRIPT_FILES})

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
set(CORONA_ASSIMP_DLL "${CORONA_SRC_DIR}/external/assimp/lib/assimp.dll")
set(CORONA_DXCOMPILER_LIB "${CORONA_SRC_DIR}/external/dxc/lib/x64/dxcompiler.lib")
set(CORONA_AFTERMATH_LIB "${CORONA_SRC_DIR}/external/GFSDK_Aftermath/lib/x64/GFSDK_Aftermath_Lib.x64.lib")
set(CORONA_AFTERMATH_DLL "${CORONA_SRC_DIR}/external/GFSDK_Aftermath/lib/x64/GFSDK_Aftermath_Lib.x64.dll")
set(CORONA_WINPIX_LIB "${CORONA_WINPIX_ROOT}/bin/x64/WinPixEventRuntime.lib")
set(CORONA_WINPIX_DLL "${CORONA_WINPIX_ROOT}/bin/x64/WinPixEventRuntime.dll")
set(CORONA_PHYSX_ROOT "${CORONA_SRC_DIR}/external/physx/physx/install/vc17win64-cpu-only/PhysX" CACHE PATH "Path to the local PhysX SDK install")
set(CORONA_PHYSX_LIB_DIR "${CORONA_PHYSX_ROOT}/bin/win.x86_64.vc143.mt/release")
set(CORONA_PHYSX_LIBS
    "${CORONA_PHYSX_LIB_DIR}/PhysX_64.lib"
    "${CORONA_PHYSX_LIB_DIR}/PhysXFoundation_64.lib"
    "${CORONA_PHYSX_LIB_DIR}/PhysXCommon_64.lib"
    "${CORONA_PHYSX_LIB_DIR}/PhysXCooking_64.lib"
)
set(CORONA_PHYSX_DLLS
    "${CORONA_PHYSX_LIB_DIR}/PhysX_64.dll"
    "${CORONA_PHYSX_LIB_DIR}/PhysXFoundation_64.dll"
    "${CORONA_PHYSX_LIB_DIR}/PhysXCommon_64.dll"
    "${CORONA_PHYSX_LIB_DIR}/PhysXCooking_64.dll"
    "${CORONA_PHYSX_LIB_DIR}/PVDRuntime_64.dll"
)
set(CORONA_REPO_STREAMLINE_SDK_ROOT "${CORONA_SRC_DIR}/external/streamline-sdk")
set(CORONA_STREAMLINE_SDK_ROOT "${CORONA_REPO_STREAMLINE_SDK_ROOT}" CACHE PATH "Path to the NVIDIA Streamline SDK")
if(CORONA_STREAMLINE_SDK_ROOT MATCHES "^[Hh]:[/\\\\]dev[/\\\\]streamline_test[/\\\\]streamline-sdk$")
    set(CORONA_STREAMLINE_SDK_ROOT "${CORONA_REPO_STREAMLINE_SDK_ROOT}" CACHE PATH "Path to the NVIDIA Streamline SDK" FORCE)
endif()
if((NOT EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/include/sl.h"
        OR NOT EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib"
        OR NOT EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/bin/x64/sl.interposer.dll")
   AND EXISTS "${CORONA_REPO_STREAMLINE_SDK_ROOT}/include/sl.h"
   AND EXISTS "${CORONA_REPO_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib"
   AND EXISTS "${CORONA_REPO_STREAMLINE_SDK_ROOT}/bin/x64/sl.interposer.dll")
    set(CORONA_STREAMLINE_SDK_ROOT "${CORONA_REPO_STREAMLINE_SDK_ROOT}" CACHE PATH "Path to the NVIDIA Streamline SDK" FORCE)
endif()
set(CORONA_STREAMLINE_LIB "")
set(CORONA_STREAMLINE_RUNTIME_DLL "${CORONA_STREAMLINE_SDK_ROOT}/bin/x64/sl.interposer.dll")
option(CORONA_USE_AFTERMATH "Enable NVIDIA Aftermath markers for D3D12 GPU crash diagnostics" ON)
set(CORONA_D3D12_AGILITY_SDK_VERSION "619" CACHE STRING "DirectX 12 Agility SDK version exported by Corona.")
set(CORONA_D3D12_AGILITY_NUGET_VERSION "1.619.2" CACHE STRING "Microsoft.Direct3D.D3D12 NuGet package version used for Agility SDK restore.")
set(CORONA_D3D12_AGILITY_NUGET_ROOT "${CORONA_SRC_DIR}/external/_packages" CACHE PATH "Directory where NuGet restores Microsoft.Direct3D.D3D12.")
set(CORONA_D3D12_AGILITY_BIN_DIR "" CACHE PATH "Directory containing D3D12Core.dll and d3d12SDKLayers.dll.")
get_property(CORONA_D3D12_AGILITY_CACHED_BIN_DIR CACHE CORONA_D3D12_AGILITY_BIN_DIR PROPERTY VALUE)
string(FIND "${CORONA_D3D12_AGILITY_CACHED_BIN_DIR}" "pylon-ngfx" CORONA_D3D12_AGILITY_PYLON_INDEX)
if(NOT CORONA_D3D12_AGILITY_PYLON_INDEX EQUAL -1)
    set(CORONA_D3D12_AGILITY_CACHED_BIN_DIR "")
endif()
if(CORONA_D3D12_AGILITY_CACHED_BIN_DIR)
    if(NOT EXISTS "${CORONA_D3D12_AGILITY_CACHED_BIN_DIR}/D3D12Core.dll"
        OR NOT EXISTS "${CORONA_D3D12_AGILITY_CACHED_BIN_DIR}/d3d12SDKLayers.dll")
        set(CORONA_D3D12_AGILITY_CACHED_BIN_DIR "")
    endif()
endif()
set(CORONA_D3D12_AGILITY_BIN_DIR "${CORONA_D3D12_AGILITY_CACHED_BIN_DIR}")
set(CORONA_D3D12_AGILITY_BIN_DIR "${CORONA_D3D12_AGILITY_BIN_DIR}" CACHE PATH "Directory containing D3D12Core.dll and d3d12SDKLayers.dll." FORCE)
if(EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/include/sl.h"
    AND EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib"
    AND EXISTS "${CORONA_STREAMLINE_RUNTIME_DLL}")
    set(CORONA_STREAMLINE_LIB "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib")
elseif(EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/include/sl.h" OR EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib")
    message(WARNING "NVIDIA Streamline SDK headers/libs were found, but runtime DLLs are missing under ${CORONA_STREAMLINE_SDK_ROOT}/bin/x64. Streamline/DLSS will be disabled to avoid a launch-time DLL failure.")
endif()

if(NOT CORONA_D3D12_AGILITY_BIN_DIR)
    file(GLOB CORONA_D3D12_AGILITY_NUGET_PACKAGES LIST_DIRECTORIES true
        "${CORONA_D3D12_AGILITY_NUGET_ROOT}/Microsoft.Direct3D.D3D12.${CORONA_D3D12_AGILITY_NUGET_VERSION}"
        "${CORONA_D3D12_AGILITY_NUGET_ROOT}/Microsoft.Direct3D.D3D12.1.${CORONA_D3D12_AGILITY_SDK_VERSION}.*"
    )
    list(REMOVE_DUPLICATES CORONA_D3D12_AGILITY_NUGET_PACKAGES)
    list(SORT CORONA_D3D12_AGILITY_NUGET_PACKAGES COMPARE NATURAL ORDER DESCENDING)

    set(CORONA_D3D12_AGILITY_CANDIDATES)
    foreach(CORONA_D3D12_AGILITY_PACKAGE IN LISTS CORONA_D3D12_AGILITY_NUGET_PACKAGES)
        list(APPEND CORONA_D3D12_AGILITY_CANDIDATES "${CORONA_D3D12_AGILITY_PACKAGE}/build/native/bin/x64")
    endforeach()

    foreach(CORONA_D3D12_AGILITY_CANDIDATE IN LISTS CORONA_D3D12_AGILITY_CANDIDATES)
        if(EXISTS "${CORONA_D3D12_AGILITY_CANDIDATE}/D3D12Core.dll" AND EXISTS "${CORONA_D3D12_AGILITY_CANDIDATE}/d3d12SDKLayers.dll")
            set(CORONA_D3D12_AGILITY_BIN_DIR "${CORONA_D3D12_AGILITY_CANDIDATE}")
            set(CORONA_D3D12_AGILITY_BIN_DIR "${CORONA_D3D12_AGILITY_BIN_DIR}" CACHE PATH "Directory containing D3D12Core.dll and d3d12SDKLayers.dll." FORCE)
            break()
        endif()
    endforeach()
endif()
set(CORONA_D3D12_AGILITY_ENABLED OFF)
if(CORONA_D3D12_AGILITY_BIN_DIR
    AND EXISTS "${CORONA_D3D12_AGILITY_BIN_DIR}/D3D12Core.dll"
    AND EXISTS "${CORONA_D3D12_AGILITY_BIN_DIR}/d3d12SDKLayers.dll")
    set(CORONA_D3D12_AGILITY_ENABLED ON)
    message(STATUS "Using DirectX 12 Agility SDK ${CORONA_D3D12_AGILITY_SDK_VERSION}: ${CORONA_D3D12_AGILITY_BIN_DIR}")
else()
    message(WARNING "DirectX 12 Agility SDK runtime was not found. Restore Microsoft.Direct3D.D3D12 ${CORONA_D3D12_AGILITY_NUGET_VERSION} under ${CORONA_D3D12_AGILITY_NUGET_ROOT}, or set CORONA_D3D12_AGILITY_BIN_DIR. SM 6.9/SER PSOs will fall back at runtime.")
endif()

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C ON CACHE BOOL "" FORCE)
add_subdirectory("${CORONA_SRC_DIR}/external/luau" "${CMAKE_CURRENT_BINARY_DIR}/luau" EXCLUDE_FROM_ALL)

foreach(required_path IN ITEMS
    "${CORONA_ASSIMP_LIB}"
    "${CORONA_ASSIMP_DLL}"
    "${CORONA_DXCOMPILER_LIB}"
    "${CORONA_AFTERMATH_LIB}"
    "${CORONA_WINPIX_LIB}"
    ${CORONA_PHYSX_LIBS}
    ${CORONA_PHYSX_DLLS}
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
    ${CORONA_SCRIPT_FILES}
)

target_include_directories(Corona PRIVATE
    "${CORONA_SRC_DIR}/external"
    "${CORONA_SRC_DIR}/external/DirectXTex July 2017/Include"
    "${CORONA_SRC_DIR}/external/assimp/include"
    "${CORONA_SRC_DIR}/external/imgui"
    "${CORONA_SRC_DIR}/external/imgui/backends"
    "${CORONA_SRC_DIR}/external/dxc/inc"
    "${CORONA_PHYSX_ROOT}/include"
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
    ${CORONA_PHYSX_LIBS}
    Luau.Compiler
    Luau.VM
    Dbghelp
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
    CORONA_D3D12_AGILITY_ENABLED=$<BOOL:${CORONA_D3D12_AGILITY_ENABLED}>
    CORONA_D3D12_AGILITY_SDK_VERSION=${CORONA_D3D12_AGILITY_SDK_VERSION}
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

if(MSVC)
    target_compile_options(Corona PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(Corona PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL>)
endif()

set_target_properties(Corona PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CORONA_OUT_DIR}"
)

add_custom_command(TARGET Corona POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CORONA_ASSIMP_DLL}"
        "${CORONA_OUT_DIR}/assimp.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CORONA_AFTERMATH_DLL}"
        "${CORONA_OUT_DIR}/GFSDK_Aftermath_Lib.x64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CORONA_WINPIX_DLL}"
        "${CORONA_OUT_DIR}/WinPixEventRuntime.dll"
)
if(CORONA_D3D12_AGILITY_ENABLED)
    add_custom_command(TARGET Corona POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CORONA_OUT_DIR}/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CORONA_D3D12_AGILITY_BIN_DIR}/D3D12Core.dll"
            "${CORONA_OUT_DIR}/D3D12/D3D12Core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CORONA_D3D12_AGILITY_BIN_DIR}/d3d12SDKLayers.dll"
            "${CORONA_OUT_DIR}/D3D12/d3d12SDKLayers.dll"
    )
endif()
foreach(CORONA_PHYSX_DLL IN LISTS CORONA_PHYSX_DLLS)
    get_filename_component(CORONA_PHYSX_DLL_NAME "${CORONA_PHYSX_DLL}" NAME)
    add_custom_command(TARGET Corona POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CORONA_PHYSX_DLL}"
            "${CORONA_OUT_DIR}/${CORONA_PHYSX_DLL_NAME}"
    )
endforeach()
if(CORONA_STREAMLINE_LIB)
    foreach(CORONA_STREAMLINE_DLL IN ITEMS sl.interposer.dll sl.common.dll sl.pcl.dll sl.dlss.dll nvngx_dlss.dll sl.dlss_d.dll nvngx_dlssd.dll)
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
