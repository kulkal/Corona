@echo off
setlocal
pushd "%~dp0bin"
start "Corona Drive City" Corona.exe --drive-city --load-map procedural_city_compiled --aa dlss-rr --gi-mode simple --no-pt-compaction --no-gbuffer-depth-prepass %*
popd
