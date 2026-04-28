param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

$content = Get-Content -LiteralPath $InputPath -Raw

$hitBuffers = @()
foreach ($name in $hitBuffers) {
    $pattern = "ByteAddressBuffer\s+$name\s*:\s*register\(t([0-9]+)\)\s*;"
    $replacement = "ByteAddressBuffer $name[] : register(t`$1);"
    $content = [regex]::Replace($content, $pattern, $replacement)
}

$hitTextures = @("AlbedoTex", "NormalTex", "RoughnessTex", "MetallicTex")
foreach ($name in $hitTextures) {
    $pattern = "Texture2D\s+$name\s*:\s*register\(t([0-9]+)\)\s*;"
    $replacement = "Texture2D $name[] : register(t`$1);"
    $content = [regex]::Replace($content, $pattern, $replacement)
}

foreach ($name in $hitTextures) {
    $pattern = "(?<![A-Za-z0-9_])$name\."
    $replacement = "$name[NonUniformResourceIndex(instanceID)]."
    $content = [regex]::Replace($content, $pattern, $replacement)
}

$outputDir = Split-Path -Parent $OutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

Set-Content -LiteralPath $OutputPath -Value $content -NoNewline
