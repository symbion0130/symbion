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

# --- PAT resolution -----------------------------------------------------
# Repo is private, so all fetches (initial irm of install.ps1, git clone,
# zip download) need a fine-grained Personal Access Token with read-only
# Contents permission. Order of preference:
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
        # Fresh fetch. Prefer git (lighter, supports future pulls). Fall
        # back to GitHub's zip if git isn't installed so this script works
        # on machines with literally nothing pre-installed.
        # Private repo: PAT goes into the clone URL as the x-access-token
        # user. GitHub accepts this format for both raw and git endpoints.
        $hasGit = $null -ne (Get-Command git -ErrorAction SilentlyContinue)

        # Auto-install git via winget if missing. Winget ships with Win10
        # 21H2+ and Win11 by default. We do this BEFORE falling back to the
        # zip download because the zip path is broken for private repos:
        # github.com/<repo>/archive/...zip 302-redirects to codeload.github.
        # com, and PowerShell strips the Authorization header on cross-host
        # redirects, so the codeload request returns 404. Installing git
        # sidesteps the whole problem because the PAT travels embedded in
        # the clone URL, not as a header.
        if (-not $hasGit -and (Get-Command winget -ErrorAction SilentlyContinue)) {
            Write-Section "Installing git (required to clone private repo)"
            try {
                & winget install --id Git.Git -e --source winget --silent --accept-package-agreements --accept-source-agreements
                # winget doesn't refresh the current shell's PATH; probe
                # the default install location and add it ourselves so the
                # very next Get-Command finds the new binary.
                $gitProbe = "$env:ProgramFiles\Git\cmd"
                if (Test-Path -LiteralPath (Join-Path $gitProbe 'git.exe')) {
                    $env:Path = "$gitProbe;$env:Path"
                }
                $hasGit = $null -ne (Get-Command git -ErrorAction SilentlyContinue)
                if ($hasGit) {
                    Write-Host "git installed: $((Get-Command git).Source)"
                } else {
                    Write-Host "[warn] winget install completed but git is still not on PATH"
                }
            } catch {
                Write-Host "[warn] winget install of git failed: $($_.Exception.Message)"
            }
        }

        if ($hasGit) {
            $cloneUrl = if ($Pat) {
                "https://x-access-token:$Pat@github.com/symbion0130/symbion.git"
            } else {
                "$RepoUrl.git"  # try anonymous (works only if repo is public)
            }
            # Echo the safe-redacted form so logs don't leak the token.
            Write-Host "git clone <repo> ($Branch)"
            & git clone --branch $Branch --depth 1 $cloneUrl $InstallDir
            if ($LASTEXITCODE -ne 0) {
                if (-not $Pat) {
                    Write-Error "git clone failed (no PAT available). Set `$env:SYMBION_PAT before running install.ps1, or place the PAT at %OneDrive%\Symbion\sync\.pat"
                } else {
                    Write-Error "git clone failed. Check the PAT is valid / not expired and has Contents:read permission on symbion0130/symbion."
                }
                exit 1
            }
            Save-PatToOneDrive $Pat
        } else {
            Write-Host "git not found -- downloading zip from $ZipUrl"
            $zipPath = Join-Path $env:TEMP "symbion-$Branch-$([Guid]::NewGuid()).zip"
            $extractDir = Join-Path $env:TEMP "symbion-extract-$([Guid]::NewGuid())"
            $zipHeaders = @{}
            if ($Pat) { $zipHeaders['Authorization'] = "token $Pat" }
            try {
                # Force TLS 1.2 for Win10 / PS 5.1 which sometimes default
                # to TLS 1.0 and get refused by github.com.
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -Uri $ZipUrl -OutFile $zipPath -UseBasicParsing -Headers $zipHeaders
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
