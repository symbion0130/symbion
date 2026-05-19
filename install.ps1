<#
.SYNOPSIS
    Symbion one-line installer for fresh Windows machines.

.DESCRIPTION
    Designed to be invoked with a single pasted command:

        irm https://raw.githubusercontent.com/symbion0130/symbion/main/install.ps1 | iex

    What it does (no other steps required from the user):
      1. Detects whether you're inside an existing clone or running remotely.
         Remote case: installs git via winget if missing (requires admin),
         then clones the repo to $env:USERPROFILE\symbion -- or a path you
         override via the SYMBION_INSTALL_DIR env var.
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
    Git branch to install. Defaults to main.

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

function Write-Section($msg) {
    Write-Host ''
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

function Test-IsElevated {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object System.Security.Principal.WindowsPrincipal($id)).IsInRole(
        [System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

# --- PAT resolution -----------------------------------------------------
# Repo is private, so the initial irm of install.ps1 and the git clone both
# need a fine-grained Personal Access Token with read-only Contents
# permission. Order of preference:
#   1. Worker-injected PAT (placeholder below gets find/replaced by the
#      Cloudflare Worker before this script is served, so the one-line
#      `irm bit.ly/symbioninstalls | iex` command works on any fresh
#      machine with no env var typing). When read directly from disk,
#      the placeholder stays literal and the StartsWith check below
#      falls through to source #2.
#   2. $env:SYMBION_PAT (set manually if not using the Worker)
#   3. %OneDrive%\Symbion\sync\.pat (cached from a prior install on any
#      machine signed into the same OneDrive account)
# We DO NOT prompt -- if no source has a PAT, the git clone fails with
# a clear error.
function Resolve-Pat {
    $injected = '__SYMBION_PAT_INJECTED__'
    if (-not $injected.StartsWith('__SYMBION')) { return $injected }
    if ($env:SYMBION_PAT) { return $env:SYMBION_PAT }
    $oneDrive = if ($env:OneDrive) { $env:OneDrive } else { $env:OneDriveConsumer }
    if ($oneDrive) {
        $cached = Join-Path $oneDrive 'Symbion\sync\.pat'
        if (Test-Path -LiteralPath $cached) {
            $val = (Get-Content -LiteralPath $cached -Raw).Trim()
            if ($val) { return $val }
        }
    }
    return $null
}

# After we know we have a working PAT, cache it to OneDrive so subsequent
# installs on this OneDrive-linked account don't need it re-pasted.
function Save-PatToOneDrive($pat) {
    if (-not $pat) { return }
    $oneDrive = if ($env:OneDrive) { $env:OneDrive } else { $env:OneDriveConsumer }
    if (-not $oneDrive) { return }
    $syncDir = Join-Path $oneDrive 'Symbion\sync'
    if (-not (Test-Path -LiteralPath $syncDir)) {
        New-Item -ItemType Directory -Path $syncDir -Force | Out-Null
    }
    $cached = Join-Path $syncDir '.pat'
    if (Test-Path -LiteralPath $cached) {
        $existing = (Get-Content -LiteralPath $cached -Raw).Trim()
        if ($existing -eq $pat) { return }  # unchanged, no-op
    }
    Set-Content -LiteralPath $cached -Value $pat -Encoding ASCII -NoNewline
    Write-Host "Cached PAT to $cached for future installs on this OneDrive account."
}

$Pat = Resolve-Pat

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
                    # Rewrite the origin URL to embed the PAT so `git pull`
                    # works without an interactive credential prompt. We
                    # restore the clean URL afterward so the token never
                    # gets persisted to .git/config across machines.
                    if ($Pat) {
                        & git remote set-url origin "https://x-access-token:$Pat@github.com/symbion0130/symbion.git"
                    }
                    & git pull --ff-only 2>&1 | Out-Null
                    if ($LASTEXITCODE -ne 0) {
                        Write-Host "[warn] git pull failed; continuing with existing files"
                    }
                    if ($Pat) {
                        & git remote set-url origin 'https://github.com/symbion0130/symbion.git'
                        Save-PatToOneDrive $Pat
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
        # Fresh fetch. Symbion is a private repo so we need git + a PAT.
        # The old zip-via-archive fallback was removed: GitHub's archive
        # URL 302-redirects to codeload.github.com and PowerShell drops
        # the Authorization header across hosts, so private-repo archive
        # downloads always 404. If the repo ever goes public, the zip
        # fallback can come back -- it's in git history.

        if (-not $Pat) {
            Write-Error @"
No GitHub PAT available. The symbion repo is private and requires a token to clone.

Provide one of:
  - `$env:SYMBION_PAT  (set before running this script)
  - %OneDrive%\Symbion\sync\.pat  (cached file, auto-picked up on this machine)
  - Bake one into the script via the Cloudflare Worker placeholder
"@
            exit 1
        }

        $hasGit = $null -ne (Get-Command git -ErrorAction SilentlyContinue)

        if (-not $hasGit) {
            if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
                Write-Error "git is not installed and winget is unavailable. Install Git from https://git-scm.com/download/win and re-run."
                exit 1
            }
            if (-not (Test-IsElevated)) {
                Write-Error @"
Git is not installed. Installing it via winget requires Administrator privileges,
and elevating from inside a piped 'irm | iex' invocation isn't reliable.

Do one of:
  1. Right-click PowerShell -> 'Run as Administrator', then re-run the install command.
  2. Install Git manually from https://git-scm.com/download/win and re-run.
"@
                exit 1
            }

            Write-Section "Installing git via winget (required to clone private repo)"
            & winget install --id Git.Git -e --source winget --silent --accept-package-agreements --accept-source-agreements
            $wingetExit = $LASTEXITCODE
            if ($wingetExit -ne 0) {
                Write-Error @"
Git install failed (winget exit code $wingetExit).

If a UAC prompt appeared and was dismissed, re-run and approve it.
Otherwise install Git manually from https://git-scm.com/download/win and re-run.
"@
                exit 1
            }
            # winget doesn't refresh the current shell's PATH; probe the
            # default install location and prepend it so the very next
            # Get-Command finds the new binary.
            $gitProbe = "$env:ProgramFiles\Git\cmd"
            if (Test-Path -LiteralPath (Join-Path $gitProbe 'git.exe')) {
                $env:Path = "$gitProbe;$env:Path"
            }
            $hasGit = $null -ne (Get-Command git -ErrorAction SilentlyContinue)
            if (-not $hasGit) {
                Write-Error "winget reported success but git is still not on PATH. Open a new shell and re-run."
                exit 1
            }
            Write-Host "git installed: $((Get-Command git).Source)"
        }

        $cloneUrl = "https://x-access-token:$Pat@github.com/symbion0130/symbion.git"
        # Echo the safe-redacted form so logs don't leak the token.
        Write-Host "git clone <repo> ($Branch)"
        & git clone --branch $Branch --depth 1 $cloneUrl $InstallDir
        if ($LASTEXITCODE -ne 0) {
            Write-Error "git clone failed. Check that the PAT is valid, not expired, and has Contents:read on symbion0130/symbion."
            exit 1
        }
        Save-PatToOneDrive $Pat
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
