<#
.SYNOPSIS
    Reverse mirror: pull from the portable D:\symbion to the local repo.

.DESCRIPTION
    Companion to sync-to-portable.ps1. The architectural model is:
      D:\symbion is the soul (code, DB, logs, .env, identity).
      Local %USERPROFILE%\symbion is just compute - a fast NTFS
      workspace that mirrors what D:\ has.

    Sync-from-portable is the START-OF-SESSION command:
      - Plug in D:\
      - Run this script
      - Local repo now matches D:\
      - Edit on C:\ for speed
      - The git post-commit hook keeps D:\ in sync as you commit

    Same exclusions as sync-to-portable on the destination side, plus
    .git is preserved on the destination (D:\'s mirror doesn't carry
    .git history, so we must not overwrite C:\'s .git folder).

.PARAMETER Source
    Where to pull from. Defaults to D:\symbion.

.PARAMETER Destination
    Where to mirror to. Defaults to %USERPROFILE%\symbion.

.PARAMETER Force
    Skip the uncommitted-changes guard. Use only when you knowingly
    want D:\ to overwrite local edits.

.EXAMPLE
    .\scripts\sync-from-portable.ps1
    Mirror D:\symbion -> %USERPROFILE%\symbion (typical start-of-session).

.EXAMPLE
    .\scripts\sync-from-portable.ps1 -Force
    Overwrite local even if there are uncommitted changes.
#>
param(
    [string] $Source = 'D:\symbion',
    [string] $Destination = (Join-Path $env:USERPROFILE 'symbion'),
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

function Write-Section($msg) {
    Write-Host ''
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

if (-not (Test-Path -LiteralPath $Source)) {
    Write-Host "ERROR: source $Source not found. Is the portable drive plugged in?" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath (Join-Path $Source 'symbion_v14.py'))) {
    Write-Host "ERROR: $Source doesn't look like a Symbion repo (no symbion_v14.py)." -ForegroundColor Red
    exit 1
}

Write-Section "Reverse-sync plan"
Write-Host "Source:      $Source"
Write-Host "Destination: $Destination"

# Uncommitted-changes guard. If C:\ has local edits that aren't committed,
# they'll be silently overwritten by the mirror. Warn unless -Force.
if (-not $Force -and (Test-Path -LiteralPath (Join-Path $Destination '.git'))) {
    Push-Location $Destination
    try {
        $dirty = & git status --porcelain 2>$null
        if ($LASTEXITCODE -eq 0 -and $dirty) {
            Write-Host ''
            Write-Host "WARNING: $Destination has uncommitted changes:" -ForegroundColor Yellow
            $dirty -split "`n" | Select-Object -First 10 | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
            $extra = (($dirty -split "`n").Count - 10)
            if ($extra -gt 0) { Write-Host "  ... and $extra more" -ForegroundColor Yellow }
            Write-Host ''
            Write-Host "Reverse-sync will OVERWRITE these with the D:\ versions." -ForegroundColor Yellow
            Write-Host "Either commit/stash first, or re-run with -Force to proceed." -ForegroundColor Yellow
            exit 1
        }
    } finally { Pop-Location }
}

# Excluded dirs: same as sync-to-portable for the regen-artifact set,
# PLUS .git (preserve local git state - D:\ doesn't carry it).
$excludeDirs = @(
    '.git',                  # CRITICAL: preserve local git history + branches + remotes
    '__pycache__',
    '.pytest_cache',
    'node_modules',          # rebuilt from package-lock.json by install-electron-app.ps1
    'win-unpacked',          # electron-builder intermediate
    '_pastes',               # web UI uploads are machine-specific
    'verify_artifacts',
    'symbion.egg-info'
    # NOTE: 'ollama-models' isn't here because it shouldn't be in either side
    # of this sync; it lives at D:\ollama-models, outside the repo tree.
)

# Files we never want to overwrite-from-D: nothing currently. .env,
# symbion.db, logs all come from D:\ - that's the point of D:\ being
# the soul.

$robocopyArgs = @(
    $Source, $Destination, '/MIR', '/Z', '/R:2', '/W:5',
    '/NFL', '/NDL', '/NP'
)
foreach ($d in $excludeDirs) { $robocopyArgs += '/XD'; $robocopyArgs += $d }

Write-Section "Running robocopy"
Write-Host "Excluded dirs: $($excludeDirs -join ', ')"
Write-Host ''
$t0 = Get-Date
& robocopy @robocopyArgs
$rc = $LASTEXITCODE
$dt = (Get-Date) - $t0
Write-Host ''
Write-Host "robocopy exit: $rc  (treating 0-7 as success)" -ForegroundColor $(if ($rc -lt 8) { 'Green' } else { 'Red' })
Write-Host "Duration: $([math]::Round($dt.TotalSeconds, 1))s"

if ($rc -ge 8) {
    Write-Host "Sync FAILED. Robocopy code >= 8 indicates real error." -ForegroundColor Red
    exit 1
}

Write-Section "Verifying destination"
$verifyFiles = @('symbion_v14.py', 'install.ps1', 'symbion.json', 'CLAUDE.md')
foreach ($f in $verifyFiles) {
    $cpath = Join-Path $Source $f
    $dpath = Join-Path $Destination $f
    if ((Test-Path $cpath) -and (Test-Path $dpath)) {
        $hSrc = (Get-FileHash $cpath -Algorithm MD5).Hash
        $hDst = (Get-FileHash $dpath -Algorithm MD5).Hash
        $status = if ($hSrc -eq $hDst) { 'MATCH' } else { 'DIFFER' }
        Write-Host ("  $status  {0}" -f $f)
    } else {
        Write-Host ("  MISS   {0}" -f $f) -ForegroundColor Red
    }
}

Write-Section "Done"
Write-Host "Local repo at $Destination now mirrors $Source." -ForegroundColor Green
Write-Host "Edit on this machine. The git post-commit hook will push" -ForegroundColor DarkGray
Write-Host "changes back to D:\ automatically after each commit." -ForegroundColor DarkGray
exit 0
