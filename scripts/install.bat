@echo off
rem Double-click launcher for install.ps1 (interactive install/uninstall menu).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
