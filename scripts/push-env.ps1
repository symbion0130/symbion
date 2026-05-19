<#
.SYNOPSIS
    Seed OneDrive with the current machine's .env so future installs on
    other machines pull keys automatically (no --setup prompt).

.DESCRIPTION
    Copies <repo>\.env to %OneDrive%\Symbion\sync\.env. install.ps1 looks
    for that file and uses it instead of prompting for API keys.

    Run this ONCE on whichever machine has the canonical .env (typically
    your dev machine where you originally pasted the keys). Re-run any
    time you rotate keys.

    Security note: anyone with access to the OneDrive account can read
    the keys. Same trust model as the DB sync (conversations are already
    there). If that's not acceptable, don't run this script -- use
    --setup on each new machine instead.

.PARAMETER Pull
    Reverse direction: copy %OneDrive%\Symbion\sync\.env into <repo>\.env.
    Useful when you want to refresh local keys from the OneDrive copy
    without re-running install.ps1.

.EXAMPLE
    .\scripts\push-env.ps1
    Push local .env to OneDrive.

.EXAMPLE
    .\scripts\push-env.ps1 -Pull
    Pull OneDrive .env into local repo.
#>
[CmdletBinding()]
param(
    [switch]$Pull
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$LocalEnv = Join-Path $RepoRoot '.env'

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { $env:OneDriveConsumer }
if (-not $oneDrive) {
    Write-Error "No OneDrive folder found (env OneDrive / OneDriveConsumer both empty). Sign in to OneDrive once so the folder exists, then retry."
    exit 1
}

$SyncDir   = Join-Path $oneDrive 'Symbion\sync'
$RemoteEnv = Join-Path $SyncDir '.env'

if ($Pull) {
    if (-not (Test-Path -LiteralPath $RemoteEnv)) {
        Write-Error "No .env in OneDrive at $RemoteEnv. Run push-env.ps1 on the source machine first."
        exit 1
    }
    Copy-Item -LiteralPath $RemoteEnv -Destination $LocalEnv -Force
    Write-Host "Pulled: $RemoteEnv -> $LocalEnv"
    exit 0
}

# Push direction
if (-not (Test-Path -LiteralPath $LocalEnv)) {
    Write-Error "No local .env at $LocalEnv. Run `python -m symbion --setup` first to write one."
    exit 1
}

if (-not (Test-Path -LiteralPath $SyncDir)) {
    New-Item -ItemType Directory -Path $SyncDir -Force | Out-Null
}

Copy-Item -LiteralPath $LocalEnv -Destination $RemoteEnv -Force
Write-Host "Pushed: $LocalEnv -> $RemoteEnv"
Write-Host ""
Write-Host "Future installs on other machines will now pick this up automatically."
Write-Host "Re-run this script any time you rotate API keys."
