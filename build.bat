@echo off
chcp 65001 >nul
setlocal
cd /d %~dp0

rem ---- On this host PATH contains a typo ("C:\indows\system32"), so System32 can be missing
rem      when the script is launched from a shell with an unusual environment. Repair it first.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%ProgramFiles%\nodejs;%PATH%"

rem ---- Visual Studio C++ toolchain (vswhere is not on PATH on this host; use full paths)
set "VS="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
)
if "%VS%"=="" (
  echo [ERROR] Visual Studio C++ toolchain not found.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] vcvars64.bat failed.
  exit /b 1
)

rem ---- Node.js is used for the shader build and the SPIR-V embedding step
where node >nul 2>nul
if errorlevel 1 (
  echo [ERROR] node.exe not found on PATH.
  exit /b 1
)

rem ---- glslang (downloaded by tools\fetch_toolchain.ps1)
if not exist "tools\glslang\bin\glslang.exe" (
  echo [ERROR] glslang not found. Run:  powershell -ExecutionPolicy Bypass -File tools\fetch_toolchain.ps1
  exit /b 1
)

rem ---- Vulkan headers
if not exist "vendor\include\vulkan\vulkan_core.h" (
  echo [ERROR] Vulkan headers not found. Run:  powershell -ExecutionPolicy Bypass -File tools\fetch_toolchain.ps1
  exit /b 1
)

echo [1/3] compiling shaders ...
node tools\build_shaders.js
if errorlevel 1 (
  echo [ERROR] shader compilation failed.
  exit /b 1
)

echo [2/3] embedding SPIR-V ...
node tools\embed.js build\spv src\spv_embedded.h
if errorlevel 1 (
  echo [ERROR] embedding failed.
  exit /b 1
)

echo [3/3] compiling vkbench.exe ...
cl /nologo /EHsc /std:c++17 /O2 /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS ^
   /I src /I vendor\include ^
   src\vk_min.cpp src\vk_ctx.cpp src\bench_common.cpp src\bench_alu.cpp src\bench_matrix.cpp ^
   src\bench_cache.cpp src\bench_mem.cpp src\report.cpp src\report_html.cpp src\main.cpp ^
   /Fe:vkbench.exe /link shell32.lib
if errorlevel 1 (
  echo [ERROR] C++ build failed.
  exit /b 1
)
del /q *.obj 2>nul

echo.
echo [OK] vkbench.exe built.
echo Usage:
echo   vkbench.exe                 interactive menu
echo   vkbench.exe --list          list Vulkan compute devices
echo   vkbench.exe --caps          capability probe only
echo   vkbench.exe --quick --open  quick run, open the HTML report
exit /b 0
