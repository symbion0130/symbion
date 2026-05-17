@echo off
REM ===========================================================================
REM Top-level shortcut: from the drive root (e.g. D:\symbion), run `symbion`
REM for the terminal REPL. `symbion --web` opens the web UI. Extra args pass
REM through.
REM
REM This wrapper pulls conversation state from OneDrive before launch and
REM pushes it back when Symbion exits, so symbion.db carries between machines.
REM To bypass sync (e.g. OneDrive offline), run scripts\start.bat directly.
REM ===========================================================================

setlocal
cd /d "%~dp0"

set PY=.python\python.exe
if not exist "%PY%" (
    echo Portable Python not found at %PY%
    echo Run scripts\bootstrap-portable.bat first.
    exit /b 1
)

"%PY%" scripts\sync.py pull
if errorlevel 1 (
    echo.
    echo [sync] pull failed -- aborting launch. To bypass sync run:
    echo   scripts\start.bat %*
    exit /b 1
)

call "%~dp0scripts\start.bat" %*
set SYMBION_EXIT=%ERRORLEVEL%

"%PY%" scripts\sync.py push

endlocal & exit /b %SYMBION_EXIT%
