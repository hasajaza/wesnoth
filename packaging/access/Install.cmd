@echo off
rem Wesnoth Access @ACCESS_VERSION@ setup. See docs\INSTALL.txt.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
