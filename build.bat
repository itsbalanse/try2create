@echo off
rem ---------------------------------------------------------------------------
rem  Builds Try2Create.exe (Release) and places it next to this script,
rem  together with the ONNX Runtime DLLs it loads at run time.
rem
rem  Needs: Visual Studio 2019/2022/2026 or the "Build Tools" with the
rem         "Desktop development with C++" workload (includes CMake).
rem         Get it with:  winget install Microsoft.VisualStudio.2022.BuildTools
rem  Usage: build.bat          (incremental)
rem         build.bat clean    (rebuild from scratch)
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=C:\Program Files (x86)"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs

set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto :novs

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :novs

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found. Install the "C++ CMake tools for Windows" component in the
    echo Visual Studio Installer, or run:  winget install Kitware.CMake
    exit /b 1
)

if /i "%~1"=="clean" if exist build-nmake rmdir /s /q build-nmake

cmake -S . -B build-nmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build-nmake
if errorlevel 1 exit /b 1

copy /y "build-nmake\bin\Try2Create.exe" "%~dp0Try2Create.exe" >nul
if errorlevel 1 (
    echo Could not copy Try2Create.exe - is the app still running?
    exit /b 1
)
if exist "build-nmake\bin\Try2Create.pdb" copy /y "build-nmake\bin\Try2Create.pdb" "%~dp0Try2Create.pdb" >nul
for %%f in (onnxruntime.dll onnxruntime_providers_shared.dll DirectML.dll) do (
    if exist "build-nmake\bin\%%f" copy /y "build-nmake\bin\%%f" "%~dp0%%f" >nul
)

echo.
echo Built %~dp0Try2Create.exe
if not exist "%~dp0models\nsfw.onnx" (
    echo Note: models\nsfw.onnx is missing, so image-based NSFW scoring is disabled ^(see README.md^).
)
exit /b 0

:novs
echo Visual Studio C++ build tools were not found.
echo Install them with:  winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
exit /b 1
