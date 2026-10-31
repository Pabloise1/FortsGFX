@echo off
rem Double-click to remove FortsGFX from Forts again.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
echo.
pause
