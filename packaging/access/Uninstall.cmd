@echo off
rem Wesnoth Access @ACCESS_VERSION@: removes the shortcuts created by Install.cmd.
rem The original Battle for Wesnoth installation was never modified, so nothing
rem else needs to be restored. Delete this folder afterwards to remove the files.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$d = [Environment]::GetFolderPath('Desktop'); $p = [Environment]::GetFolderPath('Programs');" ^
  "foreach ($f in @((Join-Path $d 'Wesnoth Access.lnk'), (Join-Path $p 'Wesnoth Access.lnk'))) { if (Test-Path $f) { Remove-Item $f; Write-Host ('Removed ' + $f) } }"
if exist "%~dp0data-path.txt" del "%~dp0data-path.txt"
echo Wesnoth Access shortcuts removed.
echo Your original Battle for Wesnoth installation and your saved games were not changed.
echo To remove Wesnoth Access completely, delete this folder: %~dp0
echo Accessibility preferences stay in the normal Wesnoth preferences file; the original game ignores them.
pause
