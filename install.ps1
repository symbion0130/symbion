<#
.SYNOPSIS
    Symbion one-line installer for fresh Windows machines.

.DESCRIPTION
    Designed to be invoked with a single pasted command:

        irm https://symbion-installer.symbion-0130.workers.dev?t=6cca038a4aeae1fb55baef15d4b5a7f0 | iex

    On a locked-down machine where the above fails with "running scripts
    is disabled on this system", use the ExecutionPolicy-bypass variant
    (same effect, scoped to a single child process -- no Set-ExecutionPolicy
    needed, no admin needed):

        powershell -ExecutionPolicy Bypass -Command "irm https://symbion-installer.symbion-0130.workers.dev?t=6cca038a4aeae1fb55baef15d4b5a7f0 | iex"

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
    irm https://symbion-installer.symbion-0130.workers.dev?t=6cca038a4aeae1fb55baef15d4b5a7f0 | iex
    Fresh-machine one-liner: clones to %USERPROFILE%\symbion, bootstraps,
    asks for API keys, launches Symbion. The Worker requires the token
    (passed as ?t= or as 'Authorization: Bearer ...') to gate access;
    the token is hard-coded here because the repo is private (anyone
    who can read install.ps1 already has clone access).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -Command "irm https://symbion-installer.symbion-0130.workers.dev?t=6cca038a4aeae1fb55baef15d4b5a7f0 | iex"
    Same as the previous example, but spawns a child PowerShell with
    ExecutionPolicy=Bypass for that one process. Use this on machines
    where the default policy is Restricted/AllSigned and the short
    form errors out with "running scripts is disabled on this system."

.EXAMPLE
    .\install.ps1
    Local invocation from inside an existing clone. Skips the download
    step, just bootstraps + sets up + launches.

.EXAMPLE
    %USERPROFILE%\symbion\scripts\install-web.cmd
    Refresh an existing install via the Worker. Equivalent to the
    Bypass one-liner above but as a .cmd shim, so it works from cmd,
    PowerShell, or Explorer double-click without ExecutionPolicy
    concerns.
#>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [string]$Branch,
    [switch]$SkipSetup,
    [switch]$SkipLaunch,
    [switch]$SkipOllama,
    [switch]$WithOllama,
    [switch]$SkipElectronApp
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
#      `irm https://symbion-installer.symbion-0130.workers.dev | iex`
#      command works on any fresh machine with no env var typing). When
#      read directly from disk, the placeholder stays literal and the
#      StartsWith check below falls through to source #2.
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

# --- Per-provider key resolution ----------------------------------------
# Mirrors the PAT injection pattern, generalised over every provider key
# the .env wants. For each key, the Cloudflare Worker rewrites the
# corresponding placeholder in-place so a fresh machine gets a working
# .env without an interactive --setup. Resolution order per key:
#   1. Worker-injected (Placeholder argument below, post-replacement)
#   2. Matching $env:<NAME> on the running machine
# Used in Phase 3 below when no existing .env and no OneDrive seed are
# present. User can swap or augment keys later via `python -m symbion
# --setup` or by editing .env directly.
function Resolve-InjectedKey {
    param(
        [Parameter(Mandatory)][string]$Placeholder,
        [Parameter(Mandatory)][string]$EnvVarName
    )
    if (-not $Placeholder.StartsWith('__SYMBION')) { return $Placeholder }
    return [Environment]::GetEnvironmentVariable($EnvVarName)
}

$AnthropicKey  = Resolve-InjectedKey -Placeholder '__SYMBION_ANTHROPIC_KEY_INJECTED__' -EnvVarName 'ANTHROPIC_API_KEY'
$BraveKey      = Resolve-InjectedKey -Placeholder '__SYMBION_BRAVE_KEY_INJECTED__'     -EnvVarName 'BRAVE_API_KEY'
$SymbionApiKey = Resolve-InjectedKey -Placeholder '__SYMBION_API_KEY_INJECTED__'       -EnvVarName 'SYMBION_API_KEY'

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
# Phase 2.5: Ollama + local models  (OPT-IN as of 2026-05-23)
#   Symbion's default symbion.json names mistral / llama3.2 / mxbai-
#   embed-large for the Ollama provider + semantic embeddings. The Ollama
#   pair is unworkable interactive on consumer hardware (p50 ~132s for
#   qwen2.5:14b; embedding pulls add ~670MB), so most fresh-machine
#   installs don't want it. Cloud providers (Anthropic, Groq, Moonshot)
#   need nothing from this phase.
#
#   SKIPPED BY DEFAULT. Opt in with -WithOllama or SYMBION_WITH_OLLAMA=1.
#   Legacy -SkipOllama / SYMBION_SKIP_OLLAMA still accepted (no-op now —
#   they don't conflict with the new default but stay for any scripts
#   that pass them explicitly).
# ------------------------------------------------------------------------
$withOllamaFlag = $WithOllama -or ($env:SYMBION_WITH_OLLAMA -and $env:SYMBION_WITH_OLLAMA -ne '0')
if (-not $withOllamaFlag) {
    Write-Section "Skipping Ollama (opt-in only — pass -WithOllama if you want local LLM)"
    Write-Host ""
    Write-Host "Symbion runs fine on cloud providers (Anthropic / Groq / Moonshot) with no Ollama setup." -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "To enable local LLM + semantic memory later, run ONE of:" -ForegroundColor Cyan
    Write-Host "  powershell -ExecutionPolicy Bypass -File $RepoDir\scripts\install-ollama.ps1" -ForegroundColor White
    Write-Host "  # ...or re-run this installer with -WithOllama" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "That pulls:" -ForegroundColor DarkGray
    Write-Host "  - Ollama runtime (winget; ~200MB)" -ForegroundColor DarkGray
    Write-Host "  - mistral + llama3.2  (responder/judge for --provider ollama)" -ForegroundColor DarkGray
    Write-Host "  - mxbai-embed-large   (~670MB; semantic memory; falls back to BM25 if absent)" -ForegroundColor DarkGray
    Write-Host ""
} else {
    Write-Section "Ollama + local models (idempotent; skips already-installed pieces)"
    $ollamaScript = Join-Path $RepoDir 'scripts\install-ollama.ps1'
    if (Test-Path -LiteralPath $ollamaScript) {
        try {
            & $ollamaScript
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[warn] Ollama setup exited with code $LASTEXITCODE -- continuing." -ForegroundColor Yellow
                Write-Host "       You can re-run later: powershell -ExecutionPolicy Bypass -File $ollamaScript"
                Write-Host "       Or use --provider anthropic / groq to skip Ollama entirely."
            }
        } catch {
            Write-Host "[warn] Ollama setup threw: $($_.Exception.Message)" -ForegroundColor Yellow
            Write-Host "       Symbion still works with --provider anthropic / groq. Re-run scripts\install-ollama.ps1 manually to retry."
        }
    } else {
        Write-Host "[note] $ollamaScript not present in this checkout; skipping. (Pull main if you want it.)"
    }
}

# ------------------------------------------------------------------------
# Phase 2.6: Electron desktop app (build + silently install)
#   Idempotent helper handles the whole flow:
#     - winget-installs Node.js LTS if missing
#     - npm install in electron/ (cached via package-lock.json mtime)
#     - npm run build:win (cached via dist installer mtime vs src mtime)
#     - silent NSIS install (cached via installed Symbion.exe mtime)
#   Skipped via -SkipElectronApp or SYMBION_SKIP_ELECTRON_APP env var.
#   On a fully-cached re-run (everything current), adds <2 seconds.
#   First-run adds ~2-3 minutes (Node install + electron download).
# ------------------------------------------------------------------------
$skipAppFlag = $SkipElectronApp -or ($env:SYMBION_SKIP_ELECTRON_APP -and $env:SYMBION_SKIP_ELECTRON_APP -ne '0')
if ($skipAppFlag) {
    Write-Section "Skipping Electron app build (-SkipElectronApp or SYMBION_SKIP_ELECTRON_APP set)"
} else {
    Write-Section "Electron desktop app (idempotent build + install)"
    $appScript = Join-Path $RepoDir 'scripts\install-electron-app.ps1'
    if (Test-Path -LiteralPath $appScript) {
        try {
            & $appScript -RepoRoot $RepoDir
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[warn] Electron app build exited with code $LASTEXITCODE -- continuing." -ForegroundColor Yellow
                Write-Host "       You can re-run later: powershell -ExecutionPolicy Bypass -File $appScript"
            }
        } catch {
            Write-Host "[warn] Electron app build threw: $($_.Exception.Message)" -ForegroundColor Yellow
            Write-Host "       The terminal symbion command still works. Re-run scripts\install-electron-app.ps1 manually to retry."
        }
    } else {
        Write-Host "[note] $appScript not present in this checkout; skipping. (Pull main if you want it.)"
    }
}

# ------------------------------------------------------------------------
# Phase 3: .env handling
#   Order of preference:
#     1. Existing <repo>\.env -- already present, leave alone
#     2. OneDrive seeded copy at %OneDrive%\Symbion\sync\.env -- copy in
#     3. Worker-injected keys -- seed .env with whichever provider keys
#        the Worker substituted (ANTHROPIC + BRAVE + SYMBION). Zero-prompt
#        and independent of OneDrive sync state, so this covers the case
#        where the new machine doesn't have OneDrive signed in / synced
#        yet. Kimi and DeepSeek deliberately omitted -- when needed,
#        seed them via OneDrive .env or manual --setup.
#     4. Interactive --setup prompt (last resort, paste keys by hand)
#   The OneDrive path remains the canonical multi-key-aware mechanism --
#   add another key locally + push-env.ps1 -> instantly available on every
#   machine. The Worker injection only covers the keys whose placeholders
#   are wired into install.ps1 + the Worker's find/replace table.
# ------------------------------------------------------------------------
$envDest = Join-Path $RepoDir '.env'
if (Test-Path -LiteralPath $envDest) {
    Write-Section ".env already present (skipping setup)"
} else {
    $oneDrive = if ($env:OneDrive) { $env:OneDrive } else { $env:OneDriveConsumer }
    $envSrc   = if ($oneDrive) { Join-Path $oneDrive 'Symbion\sync\.env' } else { $null }

    # Aggregate all installer-injected provider keys; whichever resolved
    # to non-empty above gets written into .env in one pass. Order is
    # stable for diff-friendliness.
    $injected = [ordered]@{
        ANTHROPIC_API_KEY = $AnthropicKey
        BRAVE_API_KEY     = $BraveKey
        SYMBION_API_KEY   = $SymbionApiKey
    }
    $injectedPresent = @($injected.GetEnumerator() | Where-Object { $_.Value })

    if ($envSrc -and (Test-Path -LiteralPath $envSrc)) {
        Write-Section "Pulling .env from OneDrive"
        Copy-Item -LiteralPath $envSrc -Destination $envDest -Force
        Write-Host "Copied $envSrc -> $envDest"
    } elseif ($injectedPresent.Count -gt 0) {
        Write-Section "Seeding .env with installer-injected keys"
        # UTF-8 no BOM to match the canonical .env encoding (CLAUDE.md repo
        # layout note). PowerShell 5.1's `Set-Content -Encoding utf8` would
        # write a BOM, so use the .NET API directly.
        $body = (($injectedPresent | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join "`n") + "`n"
        [System.IO.File]::WriteAllText(
            $envDest,
            $body,
            [System.Text.UTF8Encoding]::new($false))
        $names = ($injectedPresent | ForEach-Object { $_.Key }) -join ', '
        Write-Host "Wrote $envDest with: $names"
        Write-Host "Swap or add keys later by editing the file or running: python -m symbion --setup"
    } elseif (-not $SkipSetup) {
        Write-Section "API key setup (no OneDrive .env or installer-injected keys found)"
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
