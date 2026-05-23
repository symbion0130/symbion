<#
.SYNOPSIS
    Install Symbion's git hooks into the local .git/hooks/ directory.

.DESCRIPTION
    Git hooks live at .git/hooks/ which is local-only (not tracked by
    git itself). This script copies the source-of-truth hooks from
    scripts/git-hooks/ into the working .git/hooks/ location so they
    actually fire.

    Currently installs:
      - post-commit: auto-syncs to D:\symbion after each commit
        (see scripts/git-hooks/post-commit for behavior + failure modes)

    Idempotent. Re-running overwrites existing hook copies with the
    current source-of-truth version, which is what you want after
    pulling new commits that updated the hooks.

.PARAMETER RepoRoot
    Where the working tree lives. Defaults to the parent of this script.

.PARAMETER Verify
    Skip installation; just check that installed hooks match the
    source-of-truth versions in scripts/git-hooks/.

.EXAMPLE
    .\scripts\install-git-hooks.ps1
    Copy all hooks from scripts/git-hooks/ to .git/hooks/.

.EXAMPLE
    .\scripts\install-git-hooks.ps1 -Verify
    Check whether installed hooks are current.
#>
param(
    [string] $RepoRoot,
    [switch] $Verify
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
    if (-not $RepoRoot) { $RepoRoot = (Get-Location).Path }
}

$srcDir = Join-Path $RepoRoot 'scripts\git-hooks'
$dstDir = Join-Path $RepoRoot '.git\hooks'

if (-not (Test-Path -LiteralPath $srcDir)) {
    Write-Host "ERROR: source hooks dir not found: $srcDir" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path -LiteralPath $dstDir)) {
    Write-Host "ERROR: .git/hooks not found at $dstDir. Is this a git repo?" -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host "Source:      $srcDir" -ForegroundColor Cyan
Write-Host "Destination: $dstDir" -ForegroundColor Cyan
Write-Host ''

$hooks = Get-ChildItem -LiteralPath $srcDir -File
if (-not $hooks) {
    Write-Host "(no hooks in $srcDir to install)"
    exit 0
}

foreach ($h in $hooks) {
    $name = $h.Name
    $dst = Join-Path $dstDir $name
    $srcHash = (Get-FileHash $h.FullName -Algorithm MD5).Hash
    $dstHash = if (Test-Path -LiteralPath $dst) { (Get-FileHash $dst -Algorithm MD5).Hash } else { '' }

    if ($Verify) {
        if ($srcHash -eq $dstHash) {
            Write-Host "  CURRENT  $name"
        } elseif (-not $dstHash) {
            Write-Host "  MISSING  $name (run without -Verify to install)" -ForegroundColor Yellow
        } else {
            Write-Host "  STALE    $name (installed copy differs from source)" -ForegroundColor Yellow
        }
        continue
    }

    if ($srcHash -eq $dstHash) {
        Write-Host "  SKIP     $name (already current)"
    } else {
        Copy-Item -LiteralPath $h.FullName -Destination $dst -Force
        Write-Host "  INSTALL  $name" -ForegroundColor Green
    }
}

if (-not $Verify) {
    Write-Host ''
    Write-Host "Git hooks installed. They fire automatically on the matching event:" -ForegroundColor Green
    Write-Host "  post-commit -> runs sync-to-portable.ps1 after every commit" -ForegroundColor DarkGray
    Write-Host ''
    Write-Host "Pass SYMBION_HOOK_VERBOSE=1 in your shell to see full hook output." -ForegroundColor DarkGray
}
exit 0
