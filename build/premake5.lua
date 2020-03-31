-- premake5.lua

WIN_SDK_VERSION = "10.0.17763.0" 
workspace "MyToyDX12Renderer"
   configurations { "Debug", "Release" }
   platforms { "Win64"}

   filter { "platforms:Win64" }
      system "Windows"
      architecture "x64"



project "MyToyDX12Renderer"
   kind "ConsoleApp"
   language "C"
   cppdialect "C++17"
  
   includedirs { 
				"../src/external",
				".", 
				"C:/Program Files (x86)/Windows Kits/10/Include/10.0.17763.0/um",  
				"../src/external/DirectXTex July 2017/Include", 
				"../src/external/assimp/include",
				"../src/external/imgui/",
				"../src/external/imgui/examples"
               }
   libdirs 	{
            "../src/external/GFSDK_Aftermath/lib/x64", 
            -- "../src/DirectXTex July 2017/Lib 2017/Release",
            "../src/external/DirectXTex July 2017/Lib 2017/",
            "../src/external/assimp/lib",
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
      targetdir "../src/"

      systemversion( WIN_SDK_VERSION)
      staticruntime("off")
      flags { "NoPCH" } 
      files { 
      "../src/*.h",
      "../src/*.cpp",
      "../src/external/enkiTS/*.cpp",
      "../src/external/imgui/*.cpp",
      "../src/external/imgui/examples/imgui_impl_dx12.cpp",
      "../src/external/imgui/examples/imgui_impl_win32.cpp"
      }

   configuration "Release"
      debugdir("../src/")
      targetdir "../src/"
      systemversion( WIN_SDK_VERSION)
      staticruntime("off")
      flags { "NoPCH" } 
      files {
			"../src/*.h",
			"../src/*.cpp",
			"../src/external/enkiTS/*.cpp",
			"../src/external/imgui/*.cpp",
			"../src/external/imgui/examples/imgui_impl_dx12.cpp",
			"../src/external/imgui/examples/imgui_impl_win32.cpp"
			}


   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"

