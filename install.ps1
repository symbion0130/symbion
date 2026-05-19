<#
.SYNOPSIS
    Symbion one-line installer for fresh Windows machines.

.DESCRIPTION
    Designed to be invoked with a single pasted command:

        irm https://raw.githubusercontent.com/symbion0130/symbion/main/install.ps1 | iex

    What it does (no other steps required from the user):
      1. Detects whether you're inside an existing clone or running remotely.
         Remote case: clones (or downloads zip if git is absent) the repo to
         $env:USERPROFILE\symbion -- or a path you override via the
         SYMBION_INSTALL_DIR env var.
      2. Runs scripts\bootstrap-portable.bat to install portable Python +
         deps + the `symbion` command shim.
      3. Runs `python -m symbion --setup` so the user can paste API keys
         into .env interactively.
      4. Opens a NEW shell with `symbion` already running, so the user
         never has to know about "close this window and open a new one
         for PATH to refresh."

    Re-running the installer on a machine that already has Symbion just
    refreshes it (git pull if possible, otherwise warns and continues
    with existing files), re-runs bootstrap (which skips already-installed
    deps), and re-runs the shim install. Safe to run repeatedly.

.PARAMETER InstallDir
    Where to put the repo when running in remote mode. Defaults to
    $env:USERPROFILE\symbion. Override via env var SYMBION_INSTALL_DIR
    before piping to iex, e.g.:
        $env:SYMBION_INSTALL_DIR='D:\code\symbion'; irm <url> | iex
    Ignored when running from inside an existing clone.

.PARAMETER Branch
    Git branch / zip ref to install. Defaults to main.

.PARAMETER SkipSetup
    Skip the --setup API-key prompt. Useful for CI / unattended installs.

.PARAMETER SkipLaunch
    Skip the auto-launch of a new shell at the end. Useful for unattended
    installs that don't want a window popping up.

.EXAMPLE
    irm https://raw.githubusercontent.com/symbion0130/symbion/main/install.ps1 | iex
    Fresh-machine one-liner: clones to %USERPROFILE%\symbion, bootstraps,
    asks for API keys, launches Symbion.

.EXAMPLE
    .\install.ps1
    Local invocation from inside an existing clone. Skips the download
    step, just bootstraps + sets up + launches.
#>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [string]$Branch,
    [switch]$SkipSetup,
    [switch]$SkipLaunch
)

$ErrorActionPreference = 'Stop'

# Env-var fallbacks so the iex pipeline (which can't take params) is still
# overridable via `$env:SYMBION_INSTALL_DIR='...'; irm ... | iex`.
if (-not $InstallDir) { $InstallDir = $env:SYMBION_INSTALL_DIR }
if (-not $InstallDir) { $InstallDir = Join-Path $env:USERPROFILE 'symbion' }
if (-not $Branch)     { $Branch     = $env:SYMBION_INSTALL_BRANCH }
if (-not $Branch)     { $Branch     = 'main' }

$RepoUrl = 'https://github.com/symbion0130/symbion'
$ZipUrl  = "$RepoUrl/archive/refs/heads/$Branch.zip"

function Write-Section($msg) {
    Write-Host ''
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

# ------------------------------------------------------------------------
# Phase 1: Locate or fetch the repo
# ------------------------------------------------------------------------
$RepoDir = $null

# In-repo detection. $PSScriptRoot is non-empty when this file lives on
# disk (cloned + run locally). When the script comes in via `iex`, it has
# no file -- $PSScriptRoot is empty and we fall through to remote mode.
if ($PSScriptRoot -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'symbion_v14.py'))) {
    $RepoDir = $PSScriptRoot
    Write-Section "Using existing clone at $RepoDir"
} else {
    Write-Section "Installing Symbion to $InstallDir"

    if (Test-Path -LiteralPath $InstallDir) {
        $marker = Join-Path $InstallDir 'symbion_v14.py'
        if (Test-Path -LiteralPath $marker) {
            Write-Host "Symbion already present -- refreshing"
            $RepoDir = $InstallDir
            if (Test-Path -LiteralPath (Join-Path $InstallDir '.git')) {
                Push-Location $InstallDir
                try {
                    & git pull --ff-only 2>&1 | Out-Null
                    if ($LASTEXITCODE -ne 0) {
                        Write-Host "[warn] git pull failed; continuing with existing files"
                    }
                } finally {
                    Pop-Location
                }
            } else {
                Write-Host "[note] Existing directory is not a git clone; skipping update"
            }
        } else {
            Write-Error "$InstallDir exists but doesn't look like a Symbion install (symbion_v14.py missing). Choose a different -InstallDir or remove that directory and re-run."
            exit 1
        }
    } else {
        # Fresh fetch. Prefer git (lighter, supports future pulls). Fall
        # back to GitHub's zip if git isn't installed so this script works
        # on machines with literally nothing pre-installed.
        $hasGit = $null -ne (Get-Command git -ErrorAction SilentlyContinue)
        if ($hasGit) {
            Write-Host "git clone $RepoUrl ($Branch)"
            & git clone --branch $Branch --depth 1 $RepoUrl $InstallDir
            if ($LASTEXITCODE -ne 0) {
                Write-Error "git clone failed. Check network / GitHub access and retry."
                exit 1
            }
        } else {
            Write-Host "git not found -- downloading zip from $ZipUrl"
            $zipPath = Join-Path $env:TEMP "symbion-$Branch-$([Guid]::NewGuid()).zip"
            $extractDir = Join-Path $env:TEMP "symbion-extract-$([Guid]::NewGuid())"
            try {
                # Force TLS 1.2 for Win10 / PS 5.1 which sometimes default
                # to TLS 1.0 and get refused by github.com.
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -Uri $ZipUrl -OutFile $zipPath -UseBasicParsing
                Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force
                $extracted = Get-ChildItem $extractDir -Directory | Select-Object -First 1
                if (-not $extracted) {
                    Write-Error "Could not locate extracted Symbion directory in $extractDir"
                    exit 1
                }
                # Ensure parent of $InstallDir exists, then move.
                $parent = Split-Path -Parent $InstallDir
                if ($parent -and -not (Test-Path -LiteralPath $parent)) {
                    New-Item -ItemType Directory -Path $parent -Force | Out-Null
                }
                Move-Item -LiteralPath $extracted.FullName -Destination $InstallDir
            } finally {
                if (Test-Path -LiteralPath $zipPath)    { Remove-Item -LiteralPath $zipPath -Force }
                if (Test-Path -LiteralPath $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue }
            }
        }
        $RepoDir = $InstallDir
    }
}

# ------------------------------------------------------------------------
# Phase 2: Bootstrap portable Python + deps + shim
# ------------------------------------------------------------------------
Write-Section "Bootstrapping portable Python + dependencies"
$bootstrap = Join-Path $RepoDir 'scripts\bootstrap-portable.bat'
if (-not (Test-Path -LiteralPath $bootstrap)) {
    Write-Error "Bootstrap script missing at $bootstrap. The repo may be incomplete."
    exit 1
}
Push-Location $RepoDir
try {
    & $bootstrap
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Bootstrap failed (exit code $LASTEXITCODE). Check the output above."
        exit 1
    }
} finally {
    Pop-Location
}

# ------------------------------------------------------------------------
# Phase 3: .env handling
#   Order of preference:
#     1. Existing <repo>\.env -- already present, leave alone
#     2. OneDrive seeded copy at %OneDrive%\Symbion\sync\.env -- copy in
#     3. Interactive --setup prompt (last resort, paste keys by hand)
#   The OneDrive path makes the cross-machine flow zero-prompt: seed once
#   on your dev machine via scripts\push-env.ps1, then every fresh install
#   pulls keys automatically.
# ------------------------------------------------------------------------
$envDest = Join-Path $RepoDir '.env'
if (Test-Path -LiteralPath $envDest) {
    Write-Section ".env already present (skipping setup)"
} else {
    $oneDrive = if ($env:OneDrive) { $env:OneDrive } else { $env:OneDriveConsumer }
    $envSrc   = if ($oneDrive) { Join-Path $oneDrive 'Symbion\sync\.env' } else { $null }

    if ($envSrc -and (Test-Path -LiteralPath $envSrc)) {
        Write-Section "Pulling .env from OneDrive"
        Copy-Item -LiteralPath $envSrc -Destination $envDest -Force
        Write-Host "Copied $envSrc -> $envDest"
    } elseif (-not $SkipSetup) {
        Write-Section "API key setup (no OneDrive .env found)"
        Write-Host "Tip: run scripts\push-env.ps1 on your main machine to seed OneDrive so future installs skip this prompt."
        $py = Join-Path $RepoDir '.python\python.exe'
        if (Test-Path -LiteralPath $py) {
            Push-Location $RepoDir
            try {
                & $py -m symbion --setup
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "[warn] --setup exited non-zero. Re-run with: $py -m symbion --setup"
                }
            } finally {
                Pop-Location
            }
        } else {
            Write-Host "[warn] $py missing; skipping --setup. Run it manually after bootstrap finishes."
        }
    }
}

# ------------------------------------------------------------------------
# Phase 4: Launch Symbion in a new window so the user never has to know
# about the "open a new shell so PATH refreshes" thing.
# ------------------------------------------------------------------------
Write-Host ''
Write-Host '==========================================================' -ForegroundColor Green
Write-Host "  Symbion installed to: $RepoDir" -ForegroundColor Green
Write-Host '==========================================================' -ForegroundColor Green

if ($SkipLaunch) {
    Write-Host ''
    Write-Host 'Open a NEW cmd or PowerShell window, then type:  symbion'
    exit 0
}

Write-Host ''
Write-Host 'Launching Symbion in a new window...' -ForegroundColor Green
try {
    # -NoExit so the new window stays open after Symbion quits (so the user
    # can see any final output). -Command 'symbion' triggers the shim which
    # is now on User PATH; the new powershell.exe process reads PATH fresh
    # from the registry so the just-added entry is visible.
    Start-Process -FilePath 'powershell.exe' `
        -ArgumentList '-NoExit','-NoLogo','-Command','symbion' `
        -WorkingDirectory $RepoDir
    Write-Host 'Done. The new window is your Symbion session.'
} catch {
    Write-Host "[warn] Could not auto-launch: $($_.Exception.Message)"
    Write-Host 'Open a new cmd or PowerShell window and type:  symbion'
}
