@echo off
REM ===========================================================================
REM Bootstrap a portable Python install into .python\ on this drive so Symbion
REM runs on any x64 Windows machine without a system-wide Python install.
REM
REM Run once per drive. After this, scripts\start.bat is the launcher.
REM
REM What this does:
REM   1. Downloads Python 3.12 embeddable zip from python.org to .python\
REM   2. Patches python312._pth to enable site-packages (embeddable defaults
REM      to off — we need it for pip + Symbion's deps to import)
REM   3. Bootstraps pip via get-pip.py
REM   4. Installs Symbion in editable mode + web/MCP/PDF extras
REM
REM Drive size impact: Python embeddable ~10MB, deps ~50-100MB.
REM ===========================================================================

setlocal
cd /d "%~dp0.."

set PYVER=3.12.7
set PYDIR=.python
set PYZIP=python-%PYVER%-embed-amd64.zip
set PTHFILE=%PYDIR%\python312._pth

if exist "%PYDIR%\python.exe" (
    echo Portable Python already present at %PYDIR%\. Skipping download.
    goto :install_deps
)

echo.
echo === Downloading Python %PYVER% embeddable x64 ===
curl -L -o "%PYZIP%" "https://www.python.org/ftp/python/%PYVER%/python-%PYVER%-embed-amd64.zip"
if errorlevel 1 goto :err

echo.
echo === Extracting to %PYDIR%\ ===
mkdir "%PYDIR%" 2>nul
powershell -NoProfile -Command "Expand-Archive -Path '%PYZIP%' -DestinationPath '%PYDIR%' -Force"
if errorlevel 1 goto :err
del "%PYZIP%"

echo.
echo === Enabling site-packages in %PTHFILE% ===
REM Embeddable Python ships with `#import site` (commented) — uncomment it
REM so pip-installed packages are importable.
powershell -NoProfile -Command "(Get-Content '%PTHFILE%') -replace '^#import site','import site' | Set-Content '%PTHFILE%'"

echo.
echo === Bootstrapping pip ===
curl -L -o "%PYDIR%\get-pip.py" "https://bootstrap.pypa.io/get-pip.py"
if errorlevel 1 goto :err
"%PYDIR%\python.exe" "%PYDIR%\get-pip.py" --no-warn-script-location
if errorlevel 1 goto :err
del "%PYDIR%\get-pip.py"

:install_deps
echo.
echo === Installing build deps (setuptools, wheel) ===
REM Embeddable Python ships without setuptools, so PEP 517 editable builds
REM fail with "Cannot import setuptools.build_meta" unless we install it
REM upfront. Adding it to the regular site-packages also makes future
REM `pip install <thing>` calls work without requiring an isolated build env.
"%PYDIR%\python.exe" -m pip install --no-warn-script-location setuptools wheel
if errorlevel 1 goto :err

echo.
echo === Installing Symbion + extras ===
REM Editable install points back at D:\symbion (or whatever letter is mounted).
REM Drops a .pth file in .python\Lib\site-packages so the symbion package
REM imports straight from this drive — no copy, no rebuild on edits.
"%PYDIR%\python.exe" -m pip install --no-warn-script-location -e ".[web]" mcp mcp-server-time pypdf
if errorlevel 1 goto :err

echo.
echo ===========================================================================
echo OK. Portable Python ready at %PYDIR%\python.exe
echo Launch with: scripts\start.bat
echo ===========================================================================
exit /b 0

:err
echo.
echo Bootstrap FAILED. Check the error above.
exit /b 1
