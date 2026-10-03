# fetch_toolchain.ps1 -- downloads the two build-time dependencies of vk-bench:
#   1. glslang (Windows x86_64 release) -> tools\glslang\bin\glslang.exe   (GLSL -> SPIR-V)
#   2. Khronos Vulkan-Headers            -> vendor\include\vulkan\*.h       (VK_NO_PROTOTYPES)
#
# The program itself needs neither at run time (it loads vulkan-1.dll dynamically);
# these are only required to build. Known-good versions are pinned below.
#
# ASCII only on purpose: Windows PowerShell 5.1 reads BOM-less UTF-8 scripts as ANSI.
[CmdletBinding()]
param(
    [string]$GlslangTag = '16.6.0',
    [string]$GlslangAsset = 'glslang-16.6.0-windows-x86_64-release.zip'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # script lives in tools\
$tools = Join-Path $root 'tools'
$vendor = Join-Path $root 'vendor'
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'

if (-not (Test-Path $curl)) { throw "curl.exe not found at $curl" }
if (-not (Test-Path $tar))  { throw "tar.exe not found at $tar" }

function Get-File($url, $out) {
    if (Test-Path $out) {
        $len = (Get-Item $out).Length
        if ($len -gt 1MB) { Write-Host "  already present: $out ($len bytes)"; return }
    }
    Write-Host "  downloading $url"
    # -C - resumes a partial download, --retry survives flaky links
    & $curl -L -C - --retry 5 --retry-delay 2 --max-time 900 -o $out $url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
    if (-not (Test-Path $out) -or (Get-Item $out).Length -lt 1MB) { throw "download too small: $out" }
}

Write-Host '[vk-bench] toolchain check'

# ---- 1. glslang
$glslangExe = Join-Path $tools 'glslang\bin\glslang.exe'
if (Test-Path $glslangExe) {
    Write-Host "  glslang present: $glslangExe"
} else {
    $zip = Join-Path $tools $GlslangAsset
    Get-File "https://github.com/KhronosGroup/glslang/releases/download/$GlslangTag/$GlslangAsset" $zip
    Write-Host "  extracting $zip"
    New-Item -ItemType Directory -Force -Path (Join-Path $tools 'glslang') | Out-Null
    & $tar -xf $zip -C (Join-Path $tools 'glslang')
    if ($LASTEXITCODE -ne 0) { throw "extracting glslang failed" }
    if (-not (Test-Path $glslangExe)) { throw "glslang.exe not found after extraction" }
    Write-Host "  glslang ready: $glslangExe"
}

# ---- 2. Vulkan-Headers
$vkCore = Join-Path $vendor 'include\vulkan\vulkan_core.h'
if (Test-Path $vkCore) {
    Write-Host "  Vulkan headers present: $vkCore"
} else {
    $zip = Join-Path $vendor 'vulkan-headers-main.zip'
    Get-File 'https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/refs/heads/main' $zip
    Write-Host "  extracting $zip"
    New-Item -ItemType Directory -Force -Path $vendor | Out-Null
    & $tar -xf $zip -C $vendor --strip-components=1
    if ($LASTEXITCODE -ne 0) { throw "extracting Vulkan-Headers failed" }
    if (-not (Test-Path $vkCore)) { throw "vulkan_core.h not found after extraction" }
    Write-Host "  Vulkan headers ready: $vkCore"
}

Write-Host '[vk-bench] toolchain OK -- now run build.bat'
