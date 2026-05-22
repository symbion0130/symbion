<#
.SYNOPSIS
    Install Ollama + pull the models Symbion defaults to.

.DESCRIPTION
    Idempotent: skips Ollama install when ollama is already on PATH or
    at its default location, and skips each model pull when it's already
    in `ollama list`. Safe to re-run.

    Pulled models (per symbion.json defaults):
      - mistral             responder (~4.4 GB)
      - llama3.2            judge / dispatch (~2.0 GB)
      - mxbai-embed-large   embeddings for hybrid retrieval (~669 MB)

    Total fresh download: ~7 GB. On a prior-install machine where these
    are already in %USERPROFILE%\.ollama\models, the script verifies via
    `ollama list` and does nothing.

.PARAMETER SkipModels
    Just install Ollama itself, don't pull any models. Useful when you'll
    pull custom ones later or have a remote Ollama instance.

.PARAMETER Models
    Override the default model list. Comma-separated, e.g.
    "mistral,llama3.2,nomic-embed-text".

.EXAMPLE
    .\scripts\install-ollama.ps1
    Full install + pull defaults.

.EXAMPLE
    .\scripts\install-ollama.ps1 -SkipModels
    Just install Ollama. Skip the model downloads.
#>
param(
    [switch] $SkipModels,
    [string] $Models = "mistral,llama3.2,mxbai-embed-large"
)

$ErrorActionPreference = "Stop"

function Write-Section($msg) {
    Write-Host ""
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

# ------------------------------------------------------------------------
# Phase A: ensure ollama is installed + on PATH
# ------------------------------------------------------------------------
function Resolve-Ollama {
    $cmd = Get-Command ollama -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # winget installs Ollama per-user under %LOCALAPPDATA%\Programs\Ollama
    # by default. The current shell's PATH doesn't auto-refresh after a
    # winget install, so we probe the canonical location and prepend.
    $probe = Join-Path $env:LOCALAPPDATA "Programs\Ollama\ollama.exe"
    if (Test-Path -LiteralPath $probe) {
        $env:Path = (Split-Path $probe -Parent) + ";" + $env:Path
        return $probe
    }
    return $null
}

$ollama = Resolve-Ollama
if (-not $ollama) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Write-Host "winget is unavailable. Install Ollama manually from https://ollama.com/download/windows then re-run." -ForegroundColor Red
        exit 1
    }
    Write-Section "Installing Ollama via winget"
    & winget install --id Ollama.Ollama --source winget --silent --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        Write-Host "winget install failed (exit $LASTEXITCODE). Install manually from https://ollama.com/download/windows" -ForegroundColor Red
        exit 1
    }
    # Refresh PATH from the registry so the just-installed binary is
    # visible to the current process.
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("Path","User")
    $ollama = Resolve-Ollama
    if (-not $ollama) {
        Write-Host "Ollama install reported success but the binary isn't on PATH or at the default location." -ForegroundColor Red
        exit 1
    }
}
Write-Host "Ollama: $ollama"

# ------------------------------------------------------------------------
# Phase B: ensure the Ollama daemon is responding on localhost:11434
# Ollama's Windows installer registers a per-user service that auto-starts
# at login, but it may not be running mid-install (especially right after
# winget install). Start it manually if needed.
# ------------------------------------------------------------------------
function Test-OllamaApi {
    try {
        $r = Invoke-RestMethod -Uri "http://localhost:11434/api/tags" -TimeoutSec 3
        return $r
    } catch { return $null }
}

$api = Test-OllamaApi
if (-not $api) {
    Write-Section "Starting Ollama daemon"
    Start-Process -WindowStyle Hidden -FilePath $ollama -ArgumentList "serve" | Out-Null
    # Poll up to 15s for the API to come up — first start can be slow.
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $api = Test-OllamaApi
        if ($api) { break }
    }
    if (-not $api) {
        Write-Host "Ollama daemon didn't come up within 15s. Check the Ollama app, then re-run." -ForegroundColor Red
        exit 1
    }
}
Write-Host "Ollama API responding on http://localhost:11434"

# ------------------------------------------------------------------------
# Phase C: pull the default models, skipping any already present
# ------------------------------------------------------------------------
if ($SkipModels) {
    Write-Host ""
    Write-Host "SkipModels set -- not pulling any models. Done." -ForegroundColor Yellow
    exit 0
}

# Models the user wants. Comma-separated; trim whitespace.
$wantList = $Models -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ }

# What's already pulled. `ollama list` returns names like 'mistral:latest';
# normalize so 'mistral' matches 'mistral:latest'.
$haveLines = @(& $ollama list 2>$null | Select-Object -Skip 1)
$haveNames = $haveLines | ForEach-Object {
    ($_ -split "\s+")[0]
} | Where-Object { $_ }
$haveNorm = @($haveNames | ForEach-Object { ($_ -split ":")[0] })

Write-Section "Models check"
$toPull = @()
foreach ($m in $wantList) {
    $base = ($m -split ":")[0]
    if ($haveNorm -contains $base) {
        Write-Host "  [skip]  $m  (already pulled)"
    } else {
        Write-Host "  [pull]  $m"
        $toPull += $m
    }
}

if ($toPull.Count -eq 0) {
    Write-Host ""
    Write-Host "All required models already present. Done." -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host ("Pulling " + $toPull.Count + " model(s). This may take several minutes per model.") -ForegroundColor Yellow

foreach ($m in $toPull) {
    Write-Section "ollama pull $m"
    & $ollama pull $m
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[warn] pull of $m failed (exit $LASTEXITCODE) -- continuing with the rest" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Done. Verify with: ollama list" -ForegroundColor Green
