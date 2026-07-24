@echo off
REM Launch Corona in editor mode with the Sponza map.
REM Working directory is set to bin\ so the DLL search path resolves.
REM Extra args (e.g. --exit-after-frames 300 or --4k) are forwarded via %*.
cd /d "%~dp0bin"
start "Corona Editor" Corona.exe --editor --aa dlss-rr --gi-mode simple --load-map sponza --no-pt-compaction %*
