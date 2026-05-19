@echo off
REM Symbion in-repo refresh.
REM Runs install.ps1 directly from the clone this shim lives in --
REM no Worker round-trip, no fresh download. install.ps1 detects the
REM in-repo case via $PSScriptRoot and just bootstraps + sets up + launches.
REM
REM Works on machines where ExecutionPolicy blocks direct .ps1 execution
REM because this .cmd invokes powershell.exe with -ExecutionPolicy Bypass
REM scoped to the child process only -- no Set-ExecutionPolicy needed.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\install.ps1"
