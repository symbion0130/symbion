@echo off
REM ===========================================================================
REM Top-level shortcut: from the drive root (e.g. D:\symbion), run `symbion`
REM and get the terminal REPL. `symbion --web` opens the web UI. All args
REM forward to scripts\start.bat which forwards to `python -m symbion`.
REM ===========================================================================
"%~dp0scripts\start.bat" %*
