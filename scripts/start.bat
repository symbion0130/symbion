@echo off
REM ===========================================================================
REM Symbion portable launcher — runs the web app using the .python\ install
REM on this drive. Works from any machine that has the drive plugged in,
REM no system Python required.
REM
REM First-time setup: run scripts\bootstrap-portable.bat once.
REM ===========================================================================

setlocal
cd /d "%~dp0.."

set PYDIR=.python
if not exist "%PYDIR%\python.exe" (
    echo.
    echo Portable Python not found at %PYDIR%\python.exe
    echo Run scripts\bootstrap-portable.bat first (one-time setup, ~5 min).
    echo.
    exit /b 1
)

REM Forward any extra args, so `start.bat --provider anthropic` etc. work.
"%PYDIR%\python.exe" -m symbion --web %*
