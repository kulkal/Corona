-- premake5.lua
workspace "dx12_framework_premake"
   configurations { "Debug", "Release" }

project "dx12_framework"
   kind "ConsoleApp"
   language "C"
   targetdir "bin/%{cfg.buildcfg}"
   includedirs { "C:/Program Files (x86)/Windows Kits/10/Include/10.0.17763.0/um" }

   files { "../src/**.h", "../src/**.cpp" }

   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"

