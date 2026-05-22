<#
.SYNOPSIS
  Expose Symbion's local web server over HTTPS via Tailscale.

.DESCRIPTION
  Browsers require HTTPS (or localhost) to expose navigator.geolocation
  and other secure-context APIs. Tailscale's `serve` and `funnel`
  commands give you a trusted *.ts.net hostname with HTTPS already
  set up -- no certificate management. Run this once after starting
  symbion --web, and the web UI's "Share location" button starts
  working from phones / other Tailscale devices.

.PARAMETER Port
  The local port Symbion is running on. Default 8000 (matches
  SymbionConfig.web_port).

.PARAMETER Funnel
  If set, exposes the URL PUBLICLY via Tailscale Funnel (anyone with
  the URL can reach Symbion). Default is `serve`, which is tailnet-only
  (only your other Tailscale-connected devices can reach it). Funnel
  must be enabled per-node in the Tailscale admin console
  (https://login.tailscale.com/admin/dns) before this works.

.PARAMETER Off
  Tear down any Symbion serve/funnel configs and exit.

.EXAMPLE
  # Tailnet-only HTTPS (default -- your phone via Tailscale can reach it)
  .\scripts\tailscale-https.ps1

.EXAMPLE
  # Public HTTPS (anyone with the URL -- must enable funnel in admin first)
  .\scripts\tailscale-https.ps1 -Funnel

.EXAMPLE
  # Stop sharing
  .\scripts\tailscale-https.ps1 -Off
#>
param(
    [int]    $Port   = 8000,
    [switch] $Funnel,
    [switch] $Off
)

$ErrorActionPreference = "Stop"

# Resolve Tailscale CLI. On Windows it lives in Program Files; PATH may
# or may not include it depending on installer choices.
$ts = (Get-Command tailscale -ErrorAction SilentlyContinue).Source
if (-not $ts) {
    $candidate = "C:\Program Files\Tailscale\tailscale.exe"
    if (Test-Path $candidate) { $ts = $candidate }
}
if (-not $ts) {
    Write-Host "Tailscale CLI not found." -ForegroundColor Red
    Write-Host "Install from https://tailscale.com/download/windows then re-run."
    exit 1
}

if ($Off) {
    Write-Host "Resetting Tailscale serve + funnel configs..." -ForegroundColor Yellow
    & $ts serve reset 2>&1 | Out-Host
    & $ts funnel reset 2>&1 | Out-Host
    Write-Host "Done." -ForegroundColor Green
    exit 0
}

# Verify Tailscale is up + signed in. Parse status JSON for the device's
# tailnet hostname so we can print the resulting URL.
try {
    $statusJson = & $ts status --json | Out-String
    $status     = $statusJson | ConvertFrom-Json
} catch {
    Write-Host "Tailscale isn't running or you're not signed in." -ForegroundColor Red
    Write-Host "Open the Tailscale app, sign in, then re-run this script."
    exit 1
}

if (-not $status.Self -or -not $status.Self.DNSName) {
    Write-Host "Couldn't read this node's tailnet hostname from status JSON." -ForegroundColor Red
    Write-Host "Run: tailscale status   --   to debug"
    exit 1
}

$dns  = $status.Self.DNSName.TrimEnd(".")
$mode = if ($Funnel) { "funnel" } else { "serve" }

Write-Host ""
Write-Host "Pointing Tailscale $mode at http://localhost:$Port ..." -ForegroundColor Cyan

# `tailscale serve` / `tailscale funnel` are idempotent -- re-running with
# a different config just replaces. --bg detaches so the script can exit.
# Capture the combined output so we can match against Tailscale's known
# "needs to be enabled" message (it returns exit code 0 in that case,
# so $LASTEXITCODE alone won't catch it).
$args   = @($mode, "--bg", "--https=443", "http://localhost:$Port")
$output = & $ts @args 2>&1 | Out-String
Write-Host $output

$needsEnable = $output -match "is not enabled on your tailnet" -or
               $output -match "Use of Funnel requires"

if ($needsEnable) {
    Write-Host ""
    Write-Host "Tailscale $mode is not yet enabled for your tailnet." -ForegroundColor Yellow
    Write-Host "This is a one-time admin opt-in -- click through the URL above"
    Write-Host "(or this one) to enable it, then re-run this script:"
    Write-Host ""
    if ($Funnel) {
        Write-Host "  https://login.tailscale.com/admin/settings/funnel" -ForegroundColor Cyan
    } else {
        Write-Host "  https://login.tailscale.com/admin/settings/serve" -ForegroundColor Cyan
    }
    exit 1
}
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Tailscale $mode failed (exit $LASTEXITCODE)." -ForegroundColor Red
    if ($Funnel) {
        Write-Host "Funnel must also be enabled per-node:"
        Write-Host "  https://login.tailscale.com/admin/machines"
        Write-Host "Pick this device, click '...', enable 'Allow funnel'."
    }
    exit 1
}

Write-Host ""
Write-Host "Symbion is now reachable over HTTPS:" -ForegroundColor Green
Write-Host "  https://$dns" -ForegroundColor White
if ($Funnel) {
    Write-Host "  (public -- anyone with the URL can reach it)" -ForegroundColor Yellow
} else {
    Write-Host "  (tailnet-only -- only your Tailscale-signed-in devices can reach it)"
}
Write-Host ""
Write-Host "Open it on your phone (or any browser) to use the web UI."
Write-Host "Geolocation, clipboard, and other secure-context APIs will now work."
Write-Host ""
Write-Host "To stop sharing:  .\scripts\tailscale-https.ps1 -Off"
