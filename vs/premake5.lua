-- premake5.lua
workspace "dx12_framework"
   configurations { "Debug", "Release" }

project "dx12_framework"
   kind "ConsoleApp"
   language "C"
   targetdir "bin/%{cfg.buildcfg}"

   files { "../src/**.h", "../src/**.cpp" }

   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"

   filter "configurations:Release"
      defines { "NDEBUG" }
      optimize "On"