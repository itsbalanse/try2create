@echo off
rem ---------------------------------------------------------------------------
rem  Assembles a clean, copy-and-run folder (and zip) in dist\ with only what
rem  the app needs: the exe, the ONNX Runtime DLLs, the model and the README.
rem  Run build.bat first.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
if not exist Try2Create.exe (
    echo Try2Create.exe not found - run build.bat first.
    exit /b 1
)
set "OUT=%~dp0dist\Try2Create"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

copy /y Try2Create.exe "%OUT%\" >nul
for %%f in (onnxruntime.dll onnxruntime_providers_shared.dll DirectML.dll README.md) do (
    if exist "%%f" copy /y "%%f" "%OUT%\" >nul
)
if exist models (
    mkdir "%OUT%\models"
    for %%f in (models\nsfw.onnx models\nsfw.json models\people.onnx models\people.json models\people.README.txt models\aesthetic_clip.onnx models\aesthetic_anime.onnx models\aesthetic.json models\aesthetic.README.txt) do if exist "%%f" copy /y "%%f" "%OUT%\models\" >nul
)

powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%~dp0dist\Try2Create.zip' -Force"
echo.
echo Folder: %OUT%
echo Zip:    %~dp0dist\Try2Create.zip
exit /b 0
