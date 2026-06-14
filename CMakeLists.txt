cmake_minimum_required(VERSION 3.21)

set(CORONA_TARGET_PLATFORM_VALUE 0)
set(CORONA_TARGET_PLATFORM_NAME "Unknown")
set(CORONA_PLATFORM_WINDOWS OFF)
set(CORONA_PLATFORM_ANDROID OFF)
set(CORONA_PLATFORM_LINUX OFF)
set(CORONA_PLATFORM_MACOS OFF)
set(CORONA_PLATFORM_IOS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Android" OR ANDROID OR DEFINED ANDROID_ABI)
    set(CORONA_TARGET_PLATFORM_VALUE 2)
    set(CORONA_TARGET_PLATFORM_NAME "Android")
    set(CORONA_PLATFORM_ANDROID ON)
elseif(WIN32)
    set(CORONA_TARGET_PLATFORM_VALUE 1)
    set(CORONA_TARGET_PLATFORM_NAME "Windows")
    set(CORONA_PLATFORM_WINDOWS ON)
elseif(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CORONA_TARGET_PLATFORM_VALUE 5)
    set(CORONA_TARGET_PLATFORM_NAME "iOS")
    set(CORONA_PLATFORM_IOS ON)
elseif(APPLE)
    set(CORONA_TARGET_PLATFORM_VALUE 4)
    set(CORONA_TARGET_PLATFORM_NAME "macOS")
    set(CORONA_PLATFORM_MACOS ON)
elseif(UNIX)
    set(CORONA_TARGET_PLATFORM_VALUE 3)
    set(CORONA_TARGET_PLATFORM_NAME "Linux")
    set(CORONA_PLATFORM_LINUX ON)
endif()

set(CORONA_WINDOWS_SDK_VERSION "10.0.18362.0")

if(CORONA_PLATFORM_WINDOWS AND CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "${CORONA_WINDOWS_SDK_VERSION}" CACHE STRING "Windows SDK version" FORCE)
endif()

project(Corona LANGUAGES C CXX)

message(STATUS "Corona target platform: ${CORONA_TARGET_PLATFORM_NAME}")

if(NOT CORONA_PLATFORM_WINDOWS AND NOT CORONA_PLATFORM_ANDROID)
    message(FATAL_ERROR "Corona currently has active CMake target rules for Windows and Android only.")
endif()

# CORONA_DISABLE_D3D12=ON forces CORONA_HAS_D3D12=0 even on Windows, used as a
# probe for the mobile-readiness work (Tier 3). With it on, all DX12 includes,
# PIX, DXC runtime, DirectXTex, Streamline, Aftermath, etc. are expected to be
# guarded out behind their respective macros.
option(CORONA_DISABLE_D3D12 "Compile without the D3D12 backend (probe for mobile readiness)." OFF)
if(NOT CORONA_PLATFORM_WINDOWS)
    set(CORONA_DISABLE_D3D12 ON CACHE BOOL "D3D12 backend is available only on Windows." FORCE)
endif()
if(CORONA_DISABLE_D3D12)
    add_compile_definitions(CORONA_DISABLE_D3D12=1)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

find_package(Vulkan QUIET)

set(CORONA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(CORONA_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")

file(GLOB CORONA_HEADERS CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.h"
)

file(GLOB CORONA_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.cpp"
)

set(CORONA_SPINE_C_ROOT "${CORONA_SRC_DIR}/external/spine-c-3.1.07")
file(GLOB CORONA_SPINE_C_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SPINE_C_ROOT}/src/spine/*.c"
)
source_group(TREE "${CORONA_SPINE_C_ROOT}/src" PREFIX "External/spine-c" FILES ${CORONA_SPINE_C_SOURCES})

if(CORONA_DISABLE_D3D12)
    list(REMOVE_ITEM CORONA_SOURCES "${CORONA_SRC_DIR}/DX12Backend.cpp")
endif()
if(NOT CORONA_PLATFORM_WINDOWS)
    list(REMOVE_ITEM CORONA_SOURCES
        "${CORONA_SRC_DIR}/Main.cpp"
        "${CORONA_SRC_DIR}/Win32Application.cpp"
    )
endif()
if(CORONA_PLATFORM_ANDROID)
    set(CORONA_ANDROID_NATIVE_APP_GLUE_DIR "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
    if(NOT EXISTS "${CORONA_ANDROID_NATIVE_APP_GLUE_DIR}/android_native_app_glue.c")
        set(CORONA_ANDROID_NATIVE_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
    endif()
    if(NOT EXISTS "${CORONA_ANDROID_NATIVE_APP_GLUE_DIR}/android_native_app_glue.c")
        message(FATAL_ERROR "Android native_app_glue was not found in the configured NDK.")
    endif()
    list(APPEND CORONA_SOURCES
        "${CORONA_SRC_DIR}/AndroidMain.cpp"
        "${CORONA_ANDROID_NATIVE_APP_GLUE_DIR}/android_native_app_glue.c"
    )
endif()

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

set(CORONA_BACKEND_SOURCES)
if(CORONA_PLATFORM_WINDOWS)
    list(APPEND CORONA_BACKEND_SOURCES
        "${CORONA_SRC_DIR}/external/imgui/backends/imgui_impl_win32.cpp"
    )
elseif(CORONA_PLATFORM_ANDROID)
    list(APPEND CORONA_BACKEND_SOURCES
        "${CORONA_SRC_DIR}/external/imgui/backends/imgui_impl_android.cpp"
    )
endif()
if(NOT CORONA_DISABLE_D3D12)
    list(APPEND CORONA_BACKEND_SOURCES
        "${CORONA_SRC_DIR}/external/imgui/backends/imgui_impl_dx12.cpp"
    )
endif()
if(Vulkan_FOUND)
    list(APPEND CORONA_BACKEND_SOURCES
        "${CORONA_SRC_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp"
    )
endif()

set(CORONA_WINPIX_ROOT "")
if(CORONA_PLATFORM_WINDOWS AND NOT CORONA_DISABLE_D3D12)
    file(GLOB CORONA_WINPIX_PACKAGES LIST_DIRECTORIES true
        "${CMAKE_CURRENT_SOURCE_DIR}/build/packages/WinPixEventRuntime.*"
    )

    list(LENGTH CORONA_WINPIX_PACKAGES CORONA_WINPIX_PACKAGE_COUNT)
    if(CORONA_WINPIX_PACKAGE_COUNT EQUAL 0)
        message(FATAL_ERROR "WinPixEventRuntime package not found under build/packages. Restore the package before configuring with CMake.")
    endif()

    list(SORT CORONA_WINPIX_PACKAGES COMPARE NATURAL ORDER DESCENDING)
    list(GET CORONA_WINPIX_PACKAGES 0 CORONA_WINPIX_ROOT)
endif()

set(CORONA_DIRECTXTEX_LIB_DIR "${CORONA_SRC_DIR}/external/DirectXTex July 2017/Lib 2017")
set(CORONA_ASSIMP_LIB "${CORONA_SRC_DIR}/external/assimp/lib/assimp.lib")
set(CORONA_ASSIMP_DLL "${CORONA_SRC_DIR}/external/assimp/lib/assimp.dll")
set(CORONA_DXCOMPILER_LIB "${CORONA_SRC_DIR}/external/dxc/lib/x64/dxcompiler.lib")
set(CORONA_AFTERMATH_LIB "${CORONA_SRC_DIR}/external/GFSDK_Aftermath/lib/x64/GFSDK_Aftermath_Lib.x64.lib")
set(CORONA_AFTERMATH_DLL "${CORONA_SRC_DIR}/external/GFSDK_Aftermath/lib/x64/GFSDK_Aftermath_Lib.x64.dll")
set(CORONA_WINPIX_LIB "")
set(CORONA_WINPIX_DLL "")
if(CORONA_WINPIX_ROOT)
    set(CORONA_WINPIX_LIB "${CORONA_WINPIX_ROOT}/bin/x64/WinPixEventRuntime.lib")
    set(CORONA_WINPIX_DLL "${CORONA_WINPIX_ROOT}/bin/x64/WinPixEventRuntime.dll")
endif()
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
option(CORONA_ENABLE_STREAMLINE "Enable NVIDIA Streamline/DLSS integration" ON)
option(CORONA_USE_AFTERMATH "Enable NVIDIA Aftermath markers for D3D12 GPU crash diagnostics" ON)
option(CORONA_ENABLE_D3D12_AGILITY "Enable DirectX 12 Agility SDK runtime exports/copy." ON)
if(NOT CORONA_PLATFORM_WINDOWS OR CORONA_DISABLE_D3D12)
    set(CORONA_ENABLE_STREAMLINE OFF CACHE BOOL "Streamline is currently wired through the D3D12 backend." FORCE)
    set(CORONA_USE_AFTERMATH OFF CACHE BOOL "Aftermath is currently wired through the D3D12 backend." FORCE)
    set(CORONA_ENABLE_D3D12_AGILITY OFF CACHE BOOL "D3D12 Agility SDK is Windows/D3D12 only." FORCE)
endif()
set(CORONA_D3D12_AGILITY_SDK_VERSION "619" CACHE STRING "DirectX 12 Agility SDK version exported by Corona.")
set(CORONA_D3D12_AGILITY_NUGET_VERSION "1.619.2" CACHE STRING "Microsoft.Direct3D.D3D12 NuGet package version used for Agility SDK restore.")
set(CORONA_D3D12_AGILITY_NUGET_ROOT "${CORONA_SRC_DIR}/external/_packages" CACHE PATH "Directory where NuGet restores Microsoft.Direct3D.D3D12.")
set(CORONA_D3D12_AGILITY_BIN_DIR "" CACHE PATH "Directory containing D3D12Core.dll and d3d12SDKLayers.dll.")
set(CORONA_D3D12_AGILITY_ENABLED OFF)
if(CORONA_ENABLE_D3D12_AGILITY)
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
else()
    message(STATUS "DirectX 12 Agility SDK disabled by CORONA_ENABLE_D3D12_AGILITY=OFF.")
endif()
if(CORONA_ENABLE_STREAMLINE
    AND EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/include/sl.h"
    AND EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib"
    AND EXISTS "${CORONA_STREAMLINE_RUNTIME_DLL}")
    set(CORONA_STREAMLINE_LIB "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib")
elseif(CORONA_ENABLE_STREAMLINE AND (EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/include/sl.h" OR EXISTS "${CORONA_STREAMLINE_SDK_ROOT}/lib/x64/sl.interposer.lib"))
    message(WARNING "NVIDIA Streamline SDK headers/libs were found, but runtime DLLs are missing under ${CORONA_STREAMLINE_SDK_ROOT}/bin/x64. Streamline/DLSS will be disabled to avoid a launch-time DLL failure.")
endif()

if(CORONA_ENABLE_D3D12_AGILITY AND NOT CORONA_D3D12_AGILITY_BIN_DIR)
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
if(CORONA_ENABLE_D3D12_AGILITY
    AND CORONA_D3D12_AGILITY_BIN_DIR
    AND EXISTS "${CORONA_D3D12_AGILITY_BIN_DIR}/D3D12Core.dll"
    AND EXISTS "${CORONA_D3D12_AGILITY_BIN_DIR}/d3d12SDKLayers.dll")
    set(CORONA_D3D12_AGILITY_ENABLED ON)
    message(STATUS "Using DirectX 12 Agility SDK ${CORONA_D3D12_AGILITY_SDK_VERSION}: ${CORONA_D3D12_AGILITY_BIN_DIR}")
elseif(CORONA_ENABLE_D3D12_AGILITY)
    message(WARNING "DirectX 12 Agility SDK runtime was not found. Restore Microsoft.Direct3D.D3D12 ${CORONA_D3D12_AGILITY_NUGET_VERSION} under ${CORONA_D3D12_AGILITY_NUGET_ROOT}, or set CORONA_D3D12_AGILITY_BIN_DIR. SM 6.9/SER PSOs will fall back at runtime.")
endif()

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C ON CACHE BOOL "" FORCE)
add_subdirectory("${CORONA_SRC_DIR}/external/luau" "${CMAKE_CURRENT_BINARY_DIR}/luau" EXCLUDE_FROM_ALL)
if(CORONA_PLATFORM_WINDOWS AND NOT CORONA_DISABLE_D3D12)
    add_subdirectory("${CORONA_SRC_DIR}/external/nv_bvh_viewer" "${CMAKE_CURRENT_BINARY_DIR}/nv_bvh_viewer" EXCLUDE_FROM_ALL)
endif()

set(CORONA_REQUIRED_RUNTIME_PATHS)
if(CORONA_PLATFORM_WINDOWS)
    list(APPEND CORONA_REQUIRED_RUNTIME_PATHS
        "${CORONA_ASSIMP_LIB}"
        "${CORONA_ASSIMP_DLL}"
        ${CORONA_PHYSX_LIBS}
        ${CORONA_PHYSX_DLLS}
    )
    if(NOT CORONA_DISABLE_D3D12)
        list(APPEND CORONA_REQUIRED_RUNTIME_PATHS
            "${CORONA_DXCOMPILER_LIB}"
            "${CORONA_AFTERMATH_LIB}"
            "${CORONA_WINPIX_LIB}"
        )
    endif()
endif()

foreach(required_path IN LISTS CORONA_REQUIRED_RUNTIME_PATHS)
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Required dependency was not found: ${required_path}")
    endif()
endforeach()

set(CORONA_TARGET_FILES
    ${CORONA_HEADERS}
    ${CORONA_SOURCES}
    ${CORONA_SPINE_C_SOURCES}
    ${CORONA_ENKITS_SOURCES}
    ${CORONA_IMGUI_SOURCES}
    ${CORONA_BACKEND_SOURCES}
    ${CORONA_SHADER_FILES}
    ${CORONA_SCRIPT_FILES}
)

if(CORONA_PLATFORM_ANDROID)
    add_library(Corona SHARED ${CORONA_TARGET_FILES})
else()
    add_executable(Corona WIN32 ${CORONA_TARGET_FILES})
endif()

target_include_directories(Corona PRIVATE
    "${CORONA_SRC_DIR}/external"
    "${CORONA_SPINE_C_ROOT}/include"
    "${CORONA_SRC_DIR}/external/imgui"
    "${CORONA_SRC_DIR}/external/imgui/backends"
    "${CORONA_SRC_DIR}"
)
if(CORONA_PLATFORM_WINDOWS)
    target_include_directories(Corona PRIVATE
        "${CORONA_SRC_DIR}/external/DirectXTex July 2017/Include"
        "${CORONA_SRC_DIR}/external/assimp/include"
        "${CORONA_SRC_DIR}/external/dxc/inc"
        "${CORONA_PHYSX_ROOT}/include"
    )
    if(CORONA_WINPIX_ROOT)
        target_include_directories(Corona PRIVATE
            "${CORONA_WINPIX_ROOT}/Include/WinPixEventRuntime"
        )
    endif()
endif()
if(CORONA_PLATFORM_ANDROID)
    target_include_directories(Corona PRIVATE
        "${CORONA_ANDROID_NATIVE_APP_GLUE_DIR}"
    )
endif()
if(Vulkan_FOUND)
    target_include_directories(Corona PRIVATE ${Vulkan_INCLUDE_DIRS})
endif()
if(CORONA_STREAMLINE_LIB)
    target_include_directories(Corona PRIVATE
        "${CORONA_STREAMLINE_SDK_ROOT}/include"
    )
endif()

target_link_libraries(Corona PRIVATE
    Luau.Compiler
    Luau.VM
)
if(CORONA_PLATFORM_WINDOWS)
    target_link_directories(Corona PRIVATE
        "${CORONA_DIRECTXTEX_LIB_DIR}"
    )
    target_link_libraries(Corona PRIVATE
        "${CORONA_ASSIMP_LIB}"
        ${CORONA_PHYSX_LIBS}
        Dbghelp
    )
    if(NOT CORONA_DISABLE_D3D12)
        target_link_libraries(Corona PRIVATE
            # Dear ImGui's stock DX12 backend still compiles its embedded UI shaders with D3DCompile.
            # Corona runtime shaders use DXC.
            d3dcompiler
            dxguid
            "${CORONA_DXCOMPILER_LIB}"
            "${CORONA_AFTERMATH_LIB}"
            "${CORONA_WINPIX_LIB}"
            CoronaBvhViewerD3D12
        )
        if(CORONA_STREAMLINE_LIB)
            target_link_libraries(Corona PRIVATE "${CORONA_STREAMLINE_LIB}")
        else()
            target_link_libraries(Corona PRIVATE d3d12 dxgi)
        endif()
    endif()
endif()
if(Vulkan_FOUND)
    target_link_libraries(Corona PRIVATE Vulkan::Vulkan)
endif()
if(CORONA_PLATFORM_ANDROID)
    target_link_libraries(Corona PRIVATE android log dl)
    target_link_options(Corona PRIVATE "-u" "ANativeActivity_onCreate")
endif()

if(CORONA_PLATFORM_WINDOWS)
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
endif()

if(Vulkan_FOUND AND (CORONA_PLATFORM_WINDOWS OR CORONA_PLATFORM_ANDROID))
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

        macro(CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY shader_path shader_stem entry_point)
            set(vertex_output "${CORONA_VULKAN_SPIRV_DIR}/${shader_stem}_${entry_point}Vulkan.vert.spv")
            list(APPEND CORONA_VULKAN_SHADER_OUTPUTS "${vertex_output}")
            add_custom_command(
                OUTPUT "${vertex_output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CORONA_VULKAN_SPIRV_DIR}"
                COMMAND "${CORONA_VULKAN_DXC}" ${CORONA_VULKAN_DXC_COMMON_FLAGS} -fspv-target-env=vulkan1.1 -T vs_6_0 -E "${entry_point}" "${shader_path}" -Fo "${vertex_output}"
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

            # Follow #include "X.hlsli" one level so entry points that
            # live in shared headers (e.g. GBufferCommon.hlsli) are still
            # discovered by the regex matches below. DXC handles the
            # include itself at compile time; this just lets CMake see
            # the entry-point names for dispatch.
            get_filename_component(CORONA_VULKAN_SHADER_DIR_HOST "${CORONA_VULKAN_SHADER}" DIRECTORY)
            string(REGEX MATCHALL "#include[ \t]+\"[A-Za-z0-9_./-]+\\.hlsli\"" CORONA_VULKAN_INCLUDE_MATCHES "${CORONA_VULKAN_SHADER_CONTENT}")
            foreach(CORONA_VULKAN_INCLUDE_MATCH IN LISTS CORONA_VULKAN_INCLUDE_MATCHES)
                string(REGEX REPLACE "#include[ \t]+\"([A-Za-z0-9_./-]+\\.hlsli)\"" "\\1" CORONA_VULKAN_INCLUDE_NAME "${CORONA_VULKAN_INCLUDE_MATCH}")
                set(CORONA_VULKAN_INCLUDE_PATH "${CORONA_VULKAN_SHADER_DIR_HOST}/${CORONA_VULKAN_INCLUDE_NAME}")
                if(EXISTS "${CORONA_VULKAN_INCLUDE_PATH}")
                    file(READ "${CORONA_VULKAN_INCLUDE_PATH}" CORONA_VULKAN_INCLUDE_CONTENT)
                    string(APPEND CORONA_VULKAN_SHADER_CONTENT "\n${CORONA_VULKAN_INCLUDE_CONTENT}")
                endif()
            endforeach()

            if(CORONA_VULKAN_SHADER_CONTENT MATCHES [=[\[shader\("raygeneration"\)\]]=])
                CORONA_ADD_VULKAN_RT_SHADER("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}")
            endif()

            # Shaders that defines GBUFFER_OMIT_ENTRIES suppress the legacy
            # GBufferCommon.hlsli VS entry points (Spine/Skeletal/etc.) —
            # they bring their own VSMain (e.g. GrassProcedural's vertex-
            # pulling path). Skip the dispatch rules for those alternates.
            set(CORONA_VULKAN_OMIT_LEGACY_ENTRIES OFF)
            if(CORONA_VULKAN_SHADER_CONTENT MATCHES "#define[ \t]+GBUFFER_OMIT_ENTRIES")
                set(CORONA_VULKAN_OMIT_LEGACY_ENTRIES ON)
            endif()
            if(CORONA_VULKAN_SHADER_CONTENT MATCHES "VSMain[ \t\r\n]*\\(" AND CORONA_VULKAN_SHADER_CONTENT MATCHES "PSMain[ \t\r\n]*\\(")
                CORONA_ADD_VULKAN_GRAPHICS_SHADER("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}")
            endif()
            if(CORONA_VULKAN_SHADER_STEM STREQUAL "GBuffer")
                CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "VSMainBindless")
            endif()
            if(NOT CORONA_VULKAN_OMIT_LEGACY_ENTRIES AND CORONA_VULKAN_SHADER_CONTENT MATCHES "SpineVSMain[ \t\r\n]*\\(")
                CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "SpineVSMain")
            endif()
            if(NOT CORONA_VULKAN_OMIT_LEGACY_ENTRIES AND CORONA_VULKAN_SHADER_CONTENT MATCHES "SpineVsInlineVSMain[ \t\r\n]*\\(")
                CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "SpineVsInlineVSMain")
            endif()
            if(NOT CORONA_VULKAN_OMIT_LEGACY_ENTRIES AND CORONA_VULKAN_SHADER_CONTENT MATCHES "SkeletalVSMain[ \t\r\n]*\\(")
                CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "SkeletalVSMain")
            endif()
            if(NOT CORONA_VULKAN_OMIT_LEGACY_ENTRIES AND CORONA_VULKAN_SHADER_CONTENT MATCHES "SkeletalVsInlineVSMain[ \t\r\n]*\\(")
                CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "SkeletalVsInlineVSMain")
            endif()
            # Cluster VS is desktop-only — gated by GBUFFER_HAS_CLUSTER in
            # GBufferCommon.hlsli. The body of the entry is visible to the
            # regex scan via the .hlsli concat above, but it's only
            # compilable when the parent file defines GBUFFER_HAS_CLUSTER
            # before the include.
            if(NOT CORONA_VULKAN_OMIT_LEGACY_ENTRIES AND CORONA_VULKAN_SHADER_CONTENT MATCHES "SkeletalVsInlineClusterVSMain[ \t\r\n]*\\(" AND CORONA_VULKAN_SHADER_CONTENT MATCHES "#define[ \t]+GBUFFER_HAS_CLUSTER")
                CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "SkeletalVsInlineClusterVSMain")
            endif()
            if(CORONA_VULKAN_SHADER_STEM STREQUAL "GBuffer")
                CORONA_ADD_VULKAN_GRAPHICS_VERTEX_ENTRY("${CORONA_VULKAN_SHADER}" "${CORONA_VULKAN_SHADER_STEM}" "StaticInstancedVSMainBindless")
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
    CORONA_TARGET_PLATFORM=${CORONA_TARGET_PLATFORM_VALUE}
    CORONA_HAS_ASSIMP=$<BOOL:${CORONA_PLATFORM_WINDOWS}>
    CORONA_HAS_PHYSX=$<BOOL:${CORONA_PLATFORM_WINDOWS}>
    $<$<BOOL:${Vulkan_FOUND}>:CORONA_HAS_VULKAN=1>
    $<$<NOT:$<BOOL:${Vulkan_FOUND}>>:CORONA_HAS_VULKAN=0>
    $<$<BOOL:${CORONA_STREAMLINE_LIB}>:WITH_STREAMLINE=1>
    USE_AFTERMATH=$<AND:$<BOOL:${CORONA_USE_AFTERMATH}>,$<BOOL:${CORONA_PLATFORM_WINDOWS}>,$<NOT:$<BOOL:${CORONA_DISABLE_D3D12}>>>
    CORONA_D3D12_AGILITY_ENABLED=$<BOOL:${CORONA_D3D12_AGILITY_ENABLED}>
    CORONA_D3D12_AGILITY_SDK_VERSION=${CORONA_D3D12_AGILITY_SDK_VERSION}
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

if(MSVC)
    target_compile_options(Corona PRIVATE $<$<CONFIG:Release>:/Zi> $<$<CONFIG:Release>:/FS>)
    target_link_options(Corona PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL>)
endif()

# --- Experimental NVIDIA NRI RHI backend ----------------------------------
# OFF by default: NRIBackend.cpp is #if CORONA_HAS_NRI-guarded, so the stock
# build compiles it to an empty TU and links no NRI. Turn ON to link the
# vendored src/external/nri and select ERenderBackendAPI::NRI at runtime.
option(CORONA_WITH_NRI "Build the experimental NRI RHI backend (links src/external/nri)" OFF)
if(CORONA_WITH_NRI)
    if(NOT EXISTS "${CORONA_SRC_DIR}/external/nri/CMakeLists.txt")
        message(FATAL_ERROR "CORONA_WITH_NRI=ON but src/external/nri/CMakeLists.txt is missing. Clone NVIDIA-RTX/NRI into src/external/nri.")
    endif()
    # Lean first integration: D3D12 only (we wrap Corona's existing ID3D12Device),
    # validation on, vendor/optional SDKs off. VK is enabled in a later milestone.
    # Corona owns its own Agility SDK, so disable NRI's copy to avoid conflicts.
    set(NRI_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
    set(NRI_ENABLE_D3D12_SUPPORT ON CACHE BOOL "" FORCE)
    set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_VK_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NONE_SUPPORT ON CACHE BOOL "" FORCE)
    set(NRI_ENABLE_VALIDATION_SUPPORT ON CACHE BOOL "" FORCE)
    set(NRI_ENABLE_AMDAGS OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NVAPI OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "" FORCE)
    # Must stay ON: with a recent Windows SDK (10.0.26100) the system d3d12.h
    # already declares the OMM raytracing structs, which collide with NRI's own
    # definitions unless NRI uses its bundled Agility SDK headers. NRI pins
    # Agility 619 — the same major version Corona already uses — so no conflict.
    set(NRI_ENABLE_AGILITY_SDK_SUPPORT ON CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NGX_SDK OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NIS_SDK OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_IMGUI_EXTENSION ON CACHE BOOL "" FORCE)
    add_subdirectory("${CORONA_SRC_DIR}/external/nri" "${CMAKE_CURRENT_BINARY_DIR}/nri" EXCLUDE_FROM_ALL)
    target_include_directories(Corona PRIVATE "${CORONA_SRC_DIR}/external/nri/Include")
    target_link_libraries(Corona PRIVATE NRI)
    target_compile_definitions(Corona PRIVATE CORONA_HAS_NRI=1)
    message(STATUS "Corona: experimental NRI backend ENABLED (CORONA_WITH_NRI)")
endif()

set_target_properties(Corona PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CORONA_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CORONA_OUT_DIR}"
)

if(CORONA_PLATFORM_WINDOWS)
    add_custom_command(TARGET Corona POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CORONA_ASSIMP_DLL}"
            "${CORONA_OUT_DIR}/assimp.dll"
    )
    if(NOT CORONA_DISABLE_D3D12)
        add_custom_command(TARGET Corona POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CORONA_AFTERMATH_DLL}"
                "${CORONA_OUT_DIR}/GFSDK_Aftermath_Lib.x64.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CORONA_WINPIX_DLL}"
                "${CORONA_OUT_DIR}/WinPixEventRuntime.dll"
        )
    endif()
endif()
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
if(CORONA_PLATFORM_WINDOWS)
    foreach(CORONA_PHYSX_DLL IN LISTS CORONA_PHYSX_DLLS)
        get_filename_component(CORONA_PHYSX_DLL_NAME "${CORONA_PHYSX_DLL}" NAME)
        add_custom_command(TARGET Corona POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CORONA_PHYSX_DLL}"
                "${CORONA_OUT_DIR}/${CORONA_PHYSX_DLL_NAME}"
        )
    endforeach()
endif()
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

if(MSVC)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Corona)
endif()
