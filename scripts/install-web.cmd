@echo off
REM Symbion web installer shim.
REM Pipes the Worker-served install.ps1 through PowerShell's iex.
REM
REM Default install target: %USERPROFILE%\symbion (the one-liner's default).
REM To install elsewhere, set SYMBION_INSTALL_DIR first:
REM   set SYMBION_INSTALL_DIR=D:\symbion
REM   D:\symbion\scripts\install-web.cmd
REM
REM Re-running on a machine that already has Symbion just refreshes it
REM (git pull + bootstrap skip-if-installed). Safe to run repeatedly.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "irm https://symbion-installer.symbion-0130.workers.dev?t=6cca038a4aeae1fb55baef15d4b5a7f0 | iex"
