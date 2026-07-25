[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$Preset = "vs2022-x64",
    [string]$PhysXPreset = "vc17win64-cpu-only",
    [string]$VulkanSdkVersion = "1.4.341.1",

    [switch]$SkipSubmodules,
    [switch]$SkipPhysX,
    [switch]$SkipVulkanSdk,
    [switch]$ForceVulkanSdkInstall,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$NuGetExe = Join-Path $RepoRoot "tools\nuget\nuget.exe"
$DepsRoot = Join-Path $RepoRoot ".deps"
$DownloadRoot = Join-Path $DepsRoot "downloads"
$LocalVulkanRoot = Join-Path $DepsRoot "VulkanSDK\$VulkanSdkVersion"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message"
}

function Assert-Command {
    param(
        [string]$Name,
        [string]$InstallHint
    )

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name was not found. $InstallHint"
    }
}

function Invoke-External {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory = $RepoRoot
    )

    Push-Location $WorkingDirectory
    try {
        Write-Host (">> {0} {1}" -f $FilePath, ($Arguments -join " "))
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$FilePath failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

function Find-MSBuild {
    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    $vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"

    if (Test-Path $vswhere) {
        $msbuildMatches = @(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "MSBuild\**\Bin\MSBuild.exe")
        if ($LASTEXITCODE -eq 0 -and $msbuildMatches.Count -gt 0) {
            return $msbuildMatches[0]
        }
    }

    $command = Get-Command "MSBuild.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    return $null
}

function Test-VulkanSdk {
    param([string]$Root)

    if ([string]::IsNullOrWhiteSpace($Root)) {
        return $false
    }

    return (Test-Path (Join-Path $Root "Bin\dxc.exe")) -and
        (Test-Path (Join-Path $Root "Include\vulkan\vulkan.h")) -and
        (Test-Path (Join-Path $Root "Lib\vulkan-1.lib"))
}

function Resolve-VulkanSdk {
    if (-not $ForceVulkanSdkInstall) {
        if (Test-VulkanSdk $env:VULKAN_SDK) {
            return (Resolve-Path $env:VULKAN_SDK).Path
        }

        $standardRoot = "C:\VulkanSDK\$VulkanSdkVersion"
        if (Test-VulkanSdk $standardRoot) {
            return $standardRoot
        }

        if (Test-VulkanSdk $LocalVulkanRoot) {
            return $LocalVulkanRoot
        }
    }

    if ($SkipVulkanSdk) {
        return $null
    }

    New-Item -ItemType Directory -Force -Path $DownloadRoot | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path $LocalVulkanRoot -Parent) | Out-Null

    $installerName = "vulkansdk-windows-X64-$VulkanSdkVersion.exe"
    $installerPath = Join-Path $DownloadRoot $installerName
    $downloadUrl = "https://sdk.lunarg.com/sdk/download/$VulkanSdkVersion/windows/$installerName"

    if (-not (Test-Path $installerPath)) {
        Write-Host "Downloading $downloadUrl"
        Invoke-WebRequest -Uri $downloadUrl -OutFile $installerPath
    }

    Write-Host "Installing Vulkan SDK copy-only to $LocalVulkanRoot"
    Invoke-External $installerPath @(
        "--root", $LocalVulkanRoot,
        "--accept-licenses",
        "--default-answer",
        "--confirm-command",
        "install",
        "copy_only=1"
    )

    if (-not (Test-VulkanSdk $LocalVulkanRoot)) {
        throw "Vulkan SDK install did not produce the expected files under $LocalVulkanRoot"
    }

    return $LocalVulkanRoot
}

function Set-VulkanEnvironment {
    param([string]$Root)

    if ([string]::IsNullOrWhiteSpace($Root)) {
        Write-Host "Vulkan SDK was not configured; CMake will build without the Vulkan backend if possible."
        return
    }

    $env:VULKAN_SDK = $Root
    $env:VK_SDK_PATH = $Root
    $vulkanBin = Join-Path $Root "Bin"
    if (($env:PATH -split ";") -notcontains $vulkanBin) {
        $env:PATH = "$vulkanBin;$env:PATH"
    }

    Write-Host "Using Vulkan SDK: $Root"
}

function Restore-NuGetPackages {
    if (-not (Test-Path $NuGetExe)) {
        Write-Step "Downloading NuGet CLI"
        New-Item -ItemType Directory -Force (Split-Path $NuGetExe) | Out-Null
        Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $NuGetExe
    }

    Invoke-External $NuGetExe @(
        "install", "Microsoft.Direct3D.D3D12",
        "-Version", "1.619.2",
        "-OutputDirectory", "src\external\_packages",
        "-NonInteractive"
    )

    Invoke-External $NuGetExe @(
        "install", "WinPixEventRuntime",
        "-Version", "1.0.240308001",
        "-OutputDirectory", "build\packages",
        "-NonInteractive"
    )
}

function Install-PhysX {
    $physxRoot = Join-Path $RepoRoot "src\external\physx\physx"
    if (-not (Test-Path (Join-Path $physxRoot "generate_projects.bat"))) {
        throw "PhysX submodule is missing or incomplete: $physxRoot"
    }

    Invoke-External (Join-Path $physxRoot "generate_projects.bat") @($PhysXPreset) $physxRoot
    Invoke-External "cmake" @(
        "--build", "compiler\$PhysXPreset",
        "--config", "release",
        "--target", "INSTALL"
    ) $physxRoot
}

function Copy-OptionalRuntimeFromVulkanSdk {
    param([string]$Root)

    if ([string]::IsNullOrWhiteSpace($Root)) {
        return
    }

    $binDir = Join-Path $RepoRoot "bin"
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null

    foreach ($name in @("dxcompiler.dll", "dxil.dll", "vulkan-1.dll")) {
        $source = Join-Path $Root "Bin\$name"
        if (Test-Path $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $binDir $name) -Force
        }
    }
}

function Assert-BuildOutputs {
    $requiredOutputs = @(
        "bin\Corona.exe",
        "bin\CoronaMeshImport.exe",
        "bin\CoronaTextureImport.exe",
        "bin\WinPixEventRuntime.dll",
        "bin\D3D12\D3D12Core.dll",
        "bin\D3D12\d3d12SDKLayers.dll",
        "bin\PhysX_64.dll",
        "bin\PhysXFoundation_64.dll",
        "bin\PhysXCommon_64.dll",
        "bin\PhysXCooking_64.dll"
    )

    if (Test-Path (Join-Path $RepoRoot "src\external\streamline-sdk\bin\x64\sl.interposer.dll")) {
        $requiredOutputs += @(
            "bin\sl.interposer.dll",
            "bin\sl.common.dll",
            "bin\sl.pcl.dll",
            "bin\sl.dlss.dll",
            "bin\nvngx_dlss.dll",
            "bin\sl.dlss_d.dll",
            "bin\nvngx_dlssd.dll"
        )
    }

    foreach ($relativePath in $requiredOutputs) {
        $path = Join-Path $RepoRoot $relativePath
        if (-not (Test-Path $path)) {
            throw "Expected build output is missing: $relativePath"
        }
    }
}

Write-Step "Checking host requirements"
Assert-Command "git" "Install Git for Windows or run from a shell where git is available."
Assert-Command "cmake" "Install CMake and make sure it is available on PATH."

$msbuild = Find-MSBuild
if (-not $msbuild) {
    throw "MSBuild with the Visual Studio C++ toolchain was not found. Install Visual Studio 2022 or Visual Studio Build Tools with the C++ workload."
}
Write-Host "Using MSBuild: $msbuild"

if (-not $SkipSubmodules) {
    Write-Step "Updating Git submodules"
    Invoke-External "git" @("submodule", "update", "--init", "--recursive")
}

Write-Step "Configuring Vulkan SDK"
$vulkanRoot = Resolve-VulkanSdk
Set-VulkanEnvironment $vulkanRoot

Write-Step "Restoring NuGet runtime packages"
Restore-NuGetPackages

if (-not $SkipPhysX) {
    Write-Step "Generating and installing PhysX"
    Install-PhysX
}

if (-not $SkipBuild) {
    Write-Step "Configuring CMake"
    Invoke-External "cmake" @("--preset", $Preset)

    Write-Step "Building Corona"
    $buildPreset = $Configuration.ToLowerInvariant()
    Invoke-External "cmake" @("--build", "--preset", $buildPreset)

    Copy-OptionalRuntimeFromVulkanSdk $vulkanRoot
    Assert-BuildOutputs
}

Write-Step "Done"
if ($SkipBuild) {
    Write-Host "Dependency restore completed. Build was skipped."
}
else {
    Write-Host "Corona is ready at bin\Corona.exe"
}
