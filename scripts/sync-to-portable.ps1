<#
.SYNOPSIS
    Mirror the live Symbion repo to a portable destination (e.g. D:\symbion).

.DESCRIPTION
    Builds a self-contained Symbion install on a portable drive so it can
    be plugged into another Windows machine and either (a) run directly
    or (b) used as a transfer mechanism alongside the Worker one-liner.

    Two modes:
      -Lean    : Source + state only (~25 MB). New machine has to run
                 install.ps1 to bootstrap Python + Electron build.
      -Loaded  : (default) Source + state + portable .python + pre-built
                 NSIS Symbion Setup installer (~430 MB). Plug-and-play on
                 the new machine - no install step needed for the desktop
                 app, just run install.ps1 to set the .env / DB paths,
                 or even just launch Symbion.exe directly.

    Excludes regeneratable heavy artifacts (node_modules, win-unpacked,
    __pycache__, .pytest_cache, .git history) and machine-specific paths
    (_pastes web UI uploads, verify_artifacts, mock_webhook_log.jsonl).

    Personal files (resume_aaron_henson.md, Aaron_Henson_Resume.txt) are
    excluded by default - they live next to the code but aren't part of
    Symbion. Pass -IncludePersonal to override.

    Uses robocopy /MIR so the destination becomes an exact mirror of the
    source minus the exclusions. Files in the destination not in the
    source are deleted.

.PARAMETER Destination
    Where to mirror to. Defaults to D:\symbion.

.PARAMETER Lean
    Switch to lean mode (source only, ~25 MB).

.PARAMETER IncludePersonal
    Include resume/personal files in the mirror.

.PARAMETER ExcludeEnv
    Strip .env from the mirror (default: included - drive carries keys).

.EXAMPLE
    .\scripts\sync-to-portable.ps1
    Loaded mirror to D:\symbion with .env included.

.EXAMPLE
    .\scripts\sync-to-portable.ps1 -Lean -ExcludeEnv
    Minimal source-only mirror without keys.

.EXAMPLE
    .\scripts\sync-to-portable.ps1 -Destination 'E:\symbion-backup'
    Mirror to a different drive.
#>
param(
    [string] $Destination = 'D:\symbion',
    [switch] $Lean,
    [switch] $IncludePersonal,
    [switch] $ExcludeEnv
)

$ErrorActionPreference = 'Stop'

function Write-Section($msg) {
    Write-Host ''
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

# Resolve source: this script lives at <REPO>\scripts\, so REPO is the parent.
$src = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $src 'symbion_v14.py'))) {
    Write-Host "ERROR: $src doesn't look like a Symbion repo (no symbion_v14.py)." -ForegroundColor Red
    exit 1
}

Write-Section "Sync plan"
Write-Host "Source:      $src"
Write-Host "Destination: $Destination"
$mode = if ($Lean) { 'Lean (~25 MB)' } else { 'Loaded (~430 MB, includes .python + pre-built installer)' }
Write-Host "Mode:        $mode"

# Always-excluded directories: regeneratable or machine-specific.
$excludeDirs = @(
    '.git',                  # huge git metadata - fresh clone on new machine is faster
    '__pycache__',           # Python bytecode cache (regenerated automatically)
    '.pytest_cache',
    'node_modules',          # electron deps (regen via npm install)
    'win-unpacked',          # electron-builder intermediate (regen via build)
    '_pastes',               # web UI uploads (machine-specific)
    'verify_artifacts',      # Playwright PNGs from session-sync verify
    'symbion.egg-info',      # pip-install metadata (regenerated)
    'ollama-models'          # Ollama models live at D:\ollama-models now, not in repo
)

# Lean mode adds heavier dirs to the exclude list.
if ($Lean) {
    $excludeDirs += '.python'   # portable Python runtime (~244 MB)
    $excludeDirs += 'dist'      # pre-built installer (~150 MB)
}

# Always-excluded files: caches, personal content, working notes.
$excludeFiles = @(
    'mock_webhook_log.jsonl'
)
if (-not $IncludePersonal) {
    $excludeFiles += 'resume_aaron_henson.md'
    $excludeFiles += 'Aaron_Henson_Resume.txt'
}
if ($ExcludeEnv) {
    $excludeFiles += '.env'
    Write-Host "  (.env will be stripped - new machine must seed keys separately)" -ForegroundColor Yellow
} else {
    Write-Host "  (.env included - API keys travel with this drive)" -ForegroundColor Yellow
}

# Build robocopy args. /MIR mirrors and deletes anything in destination
# not in source. /Z resumable mode for large copies. /R:2 /W:5 caps retry
# behaviour so a single locked file doesn't stall the whole sync. /NFL
# /NDL keeps output readable (skip per-file/dir lines, still shows summary).
$robocopyArgs = @(
    $src, $Destination, '/MIR', '/Z', '/R:2', '/W:5',
    '/NFL', '/NDL', '/NP'
)
foreach ($d in $excludeDirs) { $robocopyArgs += '/XD'; $robocopyArgs += $d }
foreach ($f in $excludeFiles) { $robocopyArgs += '/XF'; $robocopyArgs += $f }

Write-Section "Running robocopy"
Write-Host "Excluded dirs:  $($excludeDirs -join ', ')"
Write-Host "Excluded files: $($excludeFiles -join ', ')"
Write-Host ''
$t0 = Get-Date
& robocopy @robocopyArgs
# robocopy exit codes 0-7 are non-error (0=no copy, 1=copied ok, 2=extra
# files at dest, 4=mismatched, etc). 8+ is real failure. Normalize.
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
    $p = Join-Path $Destination $f
    if (Test-Path -LiteralPath $p) {
        $sz = (Get-Item $p).Length
        Write-Host ("  OK   {0,-20} {1,10:N0} bytes" -f $f, $sz)
    } else {
        Write-Host ("  MISS {0}" -f $f) -ForegroundColor Red
    }
}
$dstSize = [math]::Round(((Get-ChildItem $Destination -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum)/1MB, 1)
Write-Host ''
Write-Host "Destination total: $dstSize MB"

Write-Section "Done"
Write-Host "Portable Symbion mirror ready at $Destination" -ForegroundColor Green
Write-Host "Plug the drive into another Windows machine and:" -ForegroundColor DarkGray
if (-not $Lean) {
    Write-Host "  - Run Symbion Setup .exe from $Destination\electron\dist\ to install the desktop app" -ForegroundColor DarkGray
    Write-Host "  - OR run $Destination\install.ps1 for the full bootstrap" -ForegroundColor DarkGray
} else {
    Write-Host "  - Run $Destination\install.ps1 to bootstrap Python + Electron build" -ForegroundColor DarkGray
}
Write-Host "Re-run this script before each transfer to keep D: in sync with C:." -ForegroundColor DarkGray
exit 0  # Normalize: robocopy leaks 0-7 status codes that this script handles internally.
