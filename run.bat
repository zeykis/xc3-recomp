@echo off
rem Starts the game in its window (no time limit). Controls: see README (keyboard or Xbox-style gamepad).
rem Set XRT_DUMP_FRAMES=300 before running to also save frames to game\frames\.
cd /d "%~dp0game"
if not exist ..\build-cmake\xc3.exe (echo Build first: run build-windows.bat & pause & exit /b 1)
..\build-cmake\xc3.exe --game . --log 1 > run-log.txt 2>&1
