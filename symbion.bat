@echo off
REM ===========================================================================
REM Top-level shortcut: from the drive root (e.g. D:\symbion), run `symbion`
REM for the terminal REPL. `symbion --web` opens the web UI. Extra args pass
REM through.
REM
REM This wrapper pulls conversation state from OneDrive before launch.
REM The push back to OneDrive is handled inside Python on shutdown (see
REM the --web shutdown path in symbion_v14.py), so the batch has nothing
REM left to run after Python exits — which means cmd.exe does NOT show
REM "Terminate batch job (Y/N)?" when you Ctrl+C the web session.
REM
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
endlocal & exit /b %ERRORLEVEL%
