<#
.SYNOPSIS
    Build and silently install the Symbion Electron desktop app.

.DESCRIPTION
    Idempotent. Skips Node.js install when already on PATH, skips npm
    install when node_modules is current, skips rebuild when the
    installer is newer than electron/main.js + assets/, and skips the
    final install when the app is already installed and matches the
    just-built version.

    Triggers:
      - winget-install Node.js LTS if missing (~30s)
      - npm install in electron/ if node_modules missing (~30s, ~150MB)
      - npm run build:win to produce dist/Symbion Setup *.exe (~60s
        first run, ~10s subsequent; downloads ~115MB Electron the
        first time then caches in %LOCALAPPDATA%\electron-builder\Cache)
      - Silent install via the NSIS installer (~5s)

    Called by install.ps1 Phase 2.6 unless -SkipElectronApp or
    $env:SYMBION_SKIP_ELECTRON_APP is set. The script accepts -Force
    to bypass the "already installed" check and reinstall.

.PARAMETER RepoRoot
    Path to the Symbion repo root. Defaults to the parent of this script.

.PARAMETER Force
    Rebuild + reinstall even when the installed app appears up to date.

.EXAMPLE
    .\scripts\install-electron-app.ps1
    Builds + installs the desktop app. Skips early when nothing has changed.

.EXAMPLE
    .\scripts\install-electron-app.ps1 -Force
    Always rebuilds and reinstalls.
#>
param(
    [string] $RepoRoot,
    [switch] $Force
)

$ErrorActionPreference = "Stop"

function Write-Section($msg) {
    Write-Host ""
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

if (-not $RepoRoot) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
    if (-not $RepoRoot) { $RepoRoot = (Get-Location).Path }
}
$electronDir = Join-Path $RepoRoot "electron"
if (-not (Test-Path -LiteralPath $electronDir)) {
    Write-Host "No electron/ directory at $electronDir -- nothing to install." -ForegroundColor Yellow
    exit 0
}

# ------------------------------------------------------------------------
# Phase A: ensure Node.js + npm are on PATH
# ------------------------------------------------------------------------
function Resolve-Node {
    $cmd = Get-Command node -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # winget puts Node under %ProgramFiles%\nodejs by default.
    foreach ($probe in @(
        "$env:ProgramFiles\nodejs\node.exe",
        "$env:LOCALAPPDATA\Programs\nodejs\node.exe"
    )) {
        if (Test-Path -LiteralPath $probe) {
            $env:Path = (Split-Path $probe -Parent) + ";" + $env:Path
            return $probe
        }
    }
    return $null
}

$node = Resolve-Node
if (-not $node) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Write-Host "winget unavailable. Install Node.js LTS from https://nodejs.org/ then re-run." -ForegroundColor Red
        exit 1
    }
    Write-Section "Installing Node.js LTS via winget"
    & winget install --id OpenJS.NodeJS.LTS --source winget --silent `
        --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Node install failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit 1
    }
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("Path","User")
    $node = Resolve-Node
    if (-not $node) {
        Write-Host "Node installed but not on PATH -- open a fresh shell + re-run." -ForegroundColor Red
        exit 1
    }
}
Write-Host "Node:  $node"
Write-Host "npm:   $((Get-Command npm).Source)"

# ------------------------------------------------------------------------
# Phase B: npm install (skip when node_modules exists + is current)
# ------------------------------------------------------------------------
$nodeModules = Join-Path $electronDir "node_modules"
$pkgLock     = Join-Path $electronDir "package-lock.json"
$pkgJson     = Join-Path $electronDir "package.json"
$needInstall = $true
if ((Test-Path $nodeModules) -and (Test-Path $pkgLock)) {
    $lockMtime = (Get-Item $pkgLock).LastWriteTime
    $modMtime  = (Get-Item $nodeModules).LastWriteTime
    if ($modMtime -ge $lockMtime) {
        Write-Host ""
        Write-Host "node_modules is current with package-lock.json -- skipping npm install"
        $needInstall = $false
    }
}
if ($needInstall) {
    Write-Section "npm install in electron/"
    Push-Location $electronDir
    # PowerShell 5.1 wraps native-executable stderr as NativeCommandError
    # records when $ErrorActionPreference='Stop' is in scope -- and that
    # preference propagates INTO npm.ps1 (the shim), where its own internal
    # `& node.exe ...` invocation then terminates on the first deprecation
    # warning npm emits. Lowering preference to 'Continue' for just the
    # npm call lets warnings flow through without crashing the script.
    # $LASTEXITCODE remains the source of truth for actual failure.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & npm install --no-audit --no-fund
        if ($LASTEXITCODE -ne 0) {
            Write-Host "npm install failed (exit $LASTEXITCODE)." -ForegroundColor Red
            exit 1
        }
    } finally {
        $ErrorActionPreference = $prevEAP
        Pop-Location
    }
}

# ------------------------------------------------------------------------
# Phase C: build NSIS installer (skip when the dist/ output is newer
# than every source file we care about)
# ------------------------------------------------------------------------
$distDir   = Join-Path $electronDir "dist"
$installer = Join-Path $distDir "Symbion Setup 0.1.0.exe"

function Latest-Mtime($paths) {
    $latest = [DateTime]::MinValue
    foreach ($p in $paths) {
        if (Test-Path -LiteralPath $p) {
            Get-ChildItem -LiteralPath $p -Recurse -File -ErrorAction SilentlyContinue |
              ForEach-Object {
                  if ($_.LastWriteTime -gt $latest) { $latest = $_.LastWriteTime }
              }
        }
    }
    return $latest
}

$srcMtime = Latest-Mtime @(
    (Join-Path $electronDir "main.js"),
    (Join-Path $electronDir "preload.js"),
    (Join-Path $electronDir "package.json"),
    (Join-Path $electronDir "assets")
)

$needBuild = $true
if ((Test-Path $installer) -and -not $Force) {
    $installerMtime = (Get-Item $installer).LastWriteTime
    if ($installerMtime -ge $srcMtime) {
        Write-Host ""
        Write-Host "dist installer is newer than sources -- skipping build"
        $needBuild = $false
    }
}
if ($needBuild) {
    Write-Section "npm run build:win"
    Push-Location $electronDir
    # Same NativeCommandError-via-Stop-preference pitfall as npm install
    # above. electron-builder also emits deprecation/info to stderr.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & npm run build:win
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Build failed (exit $LASTEXITCODE)." -ForegroundColor Red
            exit 1
        }
    } finally {
        $ErrorActionPreference = $prevEAP
        Pop-Location
    }
}

if (-not (Test-Path $installer)) {
    Write-Host "Expected installer at $installer but it's not there." -ForegroundColor Red
    exit 1
}
Write-Host "Installer: $installer  ($((Get-Item $installer).Length) bytes)"

# ------------------------------------------------------------------------
# Phase D: silently install (skip when the installed copy is current)
# ------------------------------------------------------------------------
$installRoot = Join-Path $env:LOCALAPPDATA "Programs\Symbion"
$installedExe = Join-Path $installRoot "Symbion.exe"

$needInstall = $true
if ((Test-Path $installedExe) -and -not $Force) {
    $installedMtime = (Get-Item $installedExe).LastWriteTime
    $installerMtime = (Get-Item $installer).LastWriteTime
    if ($installedMtime -ge $installerMtime) {
        Write-Host ""
        Write-Host "Installed copy is current -- skipping install"
        $needInstall = $false
    }
}

if ($needInstall) {
    Write-Section "Running NSIS installer silently"
    # Close any running instance so file replacement works
    Get-Process -Name "Symbion" -ErrorAction SilentlyContinue |
      Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    $proc = Start-Process -FilePath $installer -ArgumentList "/S" -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        Write-Host "Installer exit $($proc.ExitCode). Check $installer manually." -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "==========================================================" -ForegroundColor Green
Write-Host "  Symbion desktop app: $installedExe" -ForegroundColor Green
Write-Host "  Start Menu shortcut: $env:APPDATA\Microsoft\Windows\Start Menu\Programs\Symbion.lnk" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green
