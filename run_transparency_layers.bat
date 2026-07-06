@echo off
REM Launch Corona like editor.bat, but enable only the transparency layer overlay.
REM The CyberpunkAlleyInterp map is loaded and common\030_transparency_layers.luau
REM is enabled via --transparency-layers.
REM Extra args are forwarded after the defaults, e.g.:
REM   run_transparency_layers.bat --aa taa

setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%bin\Corona.exe"

if not exist "%EXE%" (
    echo Corona.exe was not found at "%EXE%".
    echo Build first with: cmake --build "%ROOT%out\build\vs2022-x64" --config Release --target Corona
    pause
    exit /b 1
)

cd /d "%ROOT%bin"
start "Corona Transparency Layers" "%EXE%" --editor --backend d3d12 --render-mode hybrid --aa dlss-rr --gi-mode simple --load-map CyberpunkAlleyInterp --no-pt-compaction --transparency-layers %*
