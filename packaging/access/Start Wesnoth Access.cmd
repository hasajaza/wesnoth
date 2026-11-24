@echo off
rem Starts Battle for Wesnoth with the accessibility layer (Wesnoth Access @ACCESS_VERSION@).
rem Extra options are passed on, for example: "Start Wesnoth Access.cmd" --windowed
setlocal
set "HERE=%~dp0"
if exist "%HERE%data\_main.cfg" (
    start "" "%HERE%wesnoth.exe" --accessibility %*
    exit /b 0
)
if not exist "%HERE%data-path.txt" (
    echo Wesnoth Access is not set up yet. Running setup first.
    call "%HERE%Install.cmd"
)
if not exist "%HERE%data-path.txt" (
    echo Setup did not finish. Please run Install.cmd.
    pause
    exit /b 1
)
set /p DATADIR=<"%HERE%data-path.txt"
start "" "%HERE%wesnoth.exe" --accessibility --data-dir "%DATADIR%" %*
exit /b 0
