@echo off
REM ===========================================================================
REM Symbion portable launcher — runs the terminal REPL using the .python\
REM install on this drive. Works from any machine that has the drive plugged
REM in, no system Python required.
REM
REM Default: terminal mode (PowerShell / cmd REPL).
REM Web UI:  scripts\start.bat --web        (FastAPI on http://localhost:8000)
REM
REM Extra args pass straight through, e.g.:
REM   scripts\start.bat --provider anthropic
REM   scripts\start.bat --web --port 9000
REM
REM First-time setup: run scripts\bootstrap-portable.bat once.
REM ===========================================================================

setlocal
cd /d "%~dp0.."

set PYDIR=.python
if not exist "%PYDIR%\python.exe" (
    echo.
    echo Portable Python not found at %PYDIR%\python.exe
    echo Run scripts\bootstrap-portable.bat first -- one-time setup, ~5 min.
    echo.
    exit /b 1
)

"%PYDIR%\python.exe" -m symbion %*
