-- premake5.lua

WIN_SDK_VERSION = "10.0.18362.0" 
workspace "Corona"
   configurations { "Debug", "Release" }
   platforms { "Win64"}

   filter { "platforms:Win64" }
      system "Windows"
      architecture "x64"



project "Corona"
   kind "WindowedApp"
   language "C"
   cppdialect "C++17"
  
   includedirs { 
				"../src/external",
				".", 
				"C:/Program Files (x86)/Windows Kits/10/Include/10.0.18362.0/um",  
				"../src/external/DirectXTex July 2017/Include", 
				"../src/external/assimp/include",
				"../src/external/imgui/",
				"../src/external/imgui/examples",
            "../src/external/WinPixEventRuntime/Include/WinPixEventRuntime",
            "../src/external/dxc/inc",
            "../src/external/dlss/Include",
            "../src/external/rtxgi-sdk/include",
            "../src/external/NRD/Include",
            "../src/external/NRD/Integration",
            "../src/external/NRD/Helper",
            "../src/external/NRI/Include",
            "../src/external/NRI/Include/Extensions",
            "../src/external/MathLib",
            "../src/"

               }
  
   files { 
      "../src/*.h",
      "../src/*.cpp",
      "../src/external/enkiTS/*.cpp",
      "../src/external/imgui/*.cpp",
      "../src/external/imgui/examples/imgui_impl_dx12.cpp",
      "../src/external/imgui/examples/imgui_impl_win32.cpp",
      "../src/external/rtxgi-sdk/src/*.cpp",
      "../src/external/rtxgi-sdk/src/ddgi/*.cpp",
      "../src/external/NRD/Integration/NRDIntegration.hpp",
      "../src/external/NRD/Helper/*.cpp",
      "../src/external/MathLib/*.cpp",

      }

   files {"../src/Shaders/**.hlsl"}
   filter { "files:**.hlsl" }
      flags {"ExcludeFromBuild"}


   libdirs { os.findlib("dx12") }
   
   kind "WindowedApp"

   configuration "Debug"
      debugdir("../src/")
      targetdir "../src/"

      systemversion( WIN_SDK_VERSION)
      staticruntime("off")
      flags { "NoPCH" } 
     
      libdirs  {
            "../src/external/GFSDK_Aftermath/lib/x64", 
            -- "../src/DirectXTex July 2017/Lib 2017/Release",
            "../src/external/DirectXTex July 2017/Lib 2017/",
            "../src/external/assimp/lib",
            "../src/external/WinPixEventRuntime/bin//x64",
            "../src/external/dxc/lib",
            "../src/external/dlss/Lib/x64",
            "../src/external/NRD/Lib/Debug",
            "../src/external/NRI/Lib/Debug",
            "."
         }

      links{
      "d3d12",
      "dxgi",
      "d3dcompiler",
      "dxguid",
      "GFSDK_Aftermath_Lib.x64",
      "WinPixEventRuntime",
      "nvsdk_ngx_d_dbg.lib",
      -- "DirectXTex.lib",
      "assimp.lib",
      "NRD",
      "NRI",
      "NRI_Creation",
      "NRI_D3D11",
      "NRI_D3D12",
      "NRI_Validation",
      "NRI_VK",
      }


   configuration "Release"
      debugdir("../src/")
      targetdir "../src/"
      systemversion( WIN_SDK_VERSION)
      staticruntime("off")
      flags { "NoPCH" } 
   --    files {
			-- "../src/*.h",
			-- "../src/*.cpp",
			-- "../src/external/enkiTS/*.cpp",
			-- "../src/external/imgui/*.cpp",
			-- "../src/external/imgui/examples/imgui_impl_dx12.cpp",
			-- "../src/external/imgui/examples/imgui_impl_win32.cpp",
   --       "../src/external/rtxgi-sdk/src/*.cpp",
   --       "../src/external/rtxgi-sdk/src/ddgi/*.cpp",
			-- }

      libdirs    {
            "../src/external/GFSDK_Aftermath/lib/x64", 
            -- "../src/DirectXTex July 2017/Lib 2017/Release",
            "../src/external/DirectXTex July 2017/Lib 2017/",
            "../src/external/assimp/lib",
            "../src/external/WinPixEventRuntime/bin//x64",
            "../src/external/dxc/lib",
            "../src/external/dlss/Lib/x64",
            "../src/external/NRD/Lib/Release",
            "../src/external/NRI/Lib/Release",
            "."
         }

      links{
      "d3d12",
      "dxgi",
      "d3dcompiler",
      "dxguid",
      "GFSDK_Aftermath_Lib.x64",
      "WinPixEventRuntime",
      "nvsdk_ngx_d.lib",
      -- "DirectXTex.lib",
      "assimp.lib",
      "NRD",
      "NRI",
      "NRI_Creation",
      "NRI_D3D11",
      "NRI_D3D12",
      "NRI_Validation",
      "NRI_VK",
      }



   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"

