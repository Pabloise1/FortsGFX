@echo off
rem Double-click to install FortsGFX into Forts (found through Steam).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
