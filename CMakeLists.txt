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

set(CORONA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(CORONA_OUT_DIR "${CORONA_SRC_DIR}")

file(GLOB CORONA_HEADERS CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.h"
)

file(GLOB CORONA_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/*.cpp"
)

file(GLOB CORONA_ENKITS_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/external/enkiTS/*.cpp"
)

file(GLOB CORONA_IMGUI_SOURCES CONFIGURE_DEPENDS
    "${CORONA_SRC_DIR}/external/imgui/*.cpp"
)

set(CORONA_BACKEND_SOURCES
    "${CORONA_SRC_DIR}/external/imgui/examples/imgui_impl_dx12.cpp"
    "${CORONA_SRC_DIR}/external/imgui/examples/imgui_impl_win32.cpp"
)

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
    "${CORONA_SRC_DIR}/external/imgui/examples"
    "${CORONA_SRC_DIR}/external/dxc/inc"
    "${CORONA_WINPIX_ROOT}/Include/WinPixEventRuntime"
    "${CORONA_SRC_DIR}"
)
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

target_compile_definitions(Corona PRIVATE
    IMGUI_DEFINE_MATH_OPERATORS
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
