# ===========================================================================
# Top-level shortcut: from the drive root (e.g. D:\symbion), run `symbion`
# for the terminal REPL. `symbion --web` opens the web UI. Extra args pass
# through.
#
# PowerShell flavor of symbion.bat. Reason for moving off .bat: cmd.exe
# fires "Terminate batch job (Y/N)?" on Ctrl+C inside any batch context,
# regardless of whether anything is queued after. PowerShell handles Ctrl+C
# cleanly — the running script terminates and control returns to the prompt
# with no Y/N noise.
#
# This wrapper pulls conversation state from OneDrive before launch.
# The push back is handled inside Python on shutdown via atexit
# (see the --web shutdown path in symbion_v14.py).
#
# To bypass sync (e.g. OneDrive offline), invoke scripts\start.bat directly.
# ===========================================================================

$ErrorActionPreference = 'Continue'
Set-Location -Path $PSScriptRoot

$PY = Join-Path $PSScriptRoot '.python\python.exe'
if (-not (Test-Path -LiteralPath $PY)) {
    Write-Host "Portable Python not found at $PY"
    Write-Host "Run scripts\bootstrap-portable.bat first."
    exit 1
}

# Pull OneDrive state before launch.
& $PY (Join-Path $PSScriptRoot 'scripts\sync.py') 'pull'
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "[sync] pull failed -- aborting launch. To bypass sync run:"
    Write-Host "  scripts\start.bat $args"
    exit 1
}

# Run Symbion. Python's atexit hook handles the OneDrive push on shutdown.
& $PY '-m' 'symbion' @args
exit $LASTEXITCODE
