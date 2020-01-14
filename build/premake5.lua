-- premake5.lua

WIN_SDK_VERSION = "10.0.17763.0" 
workspace "dx12_framework_premake"
   configurations { "Debug", "Release" }
   platforms { "Win64"}

   filter { "platforms:Win64" }
      system "Windows"
      architecture "x64"



project "dx12_framework"
   kind "ConsoleApp"
   language "C"
   targetdir "bin/%{cfg.buildcfg}"
   includedirs { 
               ".", 
               "C:/Program Files (x86)/Windows Kits/10/Include/10.0.17763.0/um",  
               "../src/DirectXTex July 2017/Include", 
               "../src/assimp/include",
               "../src/imgui/",
               "../src/imgui/examples"
               }
   libdirs {
            "C:/dev/dx12_wrapper/src/GFSDK_Aftermath/lib/x64", 
            -- "../src/DirectXTex July 2017/Lib 2017/Release",
            "../src/DirectXTex July 2017/Lib 2017/",
            "../src/assimp/lib",
            "."
         }
   libdirs { os.findlib("dx12") }
   links{
      "d3d12",
      "dxgi",
      "d3dcompiler",
      "dxguid",
      "GFSDK_Aftermath_Lib.x64",
      -- "DirectXTex.lib",
      "assimp.lib"
      }

   kind "WindowedApp"

   configuration "Debug"
      debugdir("../src/")
      systemversion( WIN_SDK_VERSION)
      staticruntime("off")
      flags { "NoPCH" } 
      files { 
      "../src/*.h",
      "../src/*.cpp",
      "../src/enkiTS/*.cpp",
      "../src/imgui/*.cpp",
      "../src/imgui/examples/imgui_impl_dx12.cpp",
      "../src/imgui/examples/imgui_impl_win32.cpp"
      }

   configuration "Release"
      debugdir("../src/")
      systemversion( WIN_SDK_VERSION)
      staticruntime("off")
      flags { "NoPCH" } 
      files {
         "../src/*.h",
      "../src/*.cpp",
      "../src/enkiTS/*.cpp",
      "../src/imgui/*.cpp",
      "../src/imgui/examples/imgui_impl_dx12.cpp",
      "../src/imgui/examples/imgui_impl_win32.cpp"
      }


   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"

