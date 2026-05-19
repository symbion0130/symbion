<#
.SYNOPSIS
    Make `symbion` callable from any shell, any directory.

.DESCRIPTION
    Drops a symbion.cmd shim into %LOCALAPPDATA%\Programs\symbion-cli\ and
    adds that directory to the User PATH if it isn't already there. The
    shim does `call "<repo>\symbion.bat" %*` so typing `symbion` from any
    cmd OR PowerShell session invokes this clone's launcher.

    .cmd is used (not .ps1) because cmd doesn't have an ExecutionPolicy
    concept. A fresh Windows install ships with PowerShell ExecutionPolicy
    set to Restricted, which blocks every .ps1 file by default -- including
    a function registered in $PROFILE that delegates to symbion.ps1. The
    .cmd shim bypasses that whole problem.

    We DO still set CurrentUser ExecutionPolicy to RemoteSigned (if it's
    Restricted) so that symbion.ps1 itself can run when called from the
    .cmd chain (the launcher is .bat, not .ps1, so this is belt-and-
    suspenders for users who prefer to invoke .\symbion.ps1 directly).

    Per-machine: the shim hard-codes THIS clone's path. Re-run install-cli
    from a different clone on a different machine and the shim there
    points to that location -- no absolute paths committed to git.

    Cleanup: also removes the legacy profile-function block from prior
    install-cli versions, so upgrading users don't end up with two
    `symbion` definitions racing each other.

.PARAMETER Uninstall
    Remove the shim dir, the User PATH entry, and any legacy profile
    block. Does not touch ExecutionPolicy.

.EXAMPLE
    .\scripts\install-cli.ps1
    Installs/updates the shim. Open a NEW shell and run `symbion`.

.EXAMPLE
    .\scripts\install-cli.ps1 -Uninstall
    Removes the shim and PATH entry.
#>
[CmdletBinding()]
param(
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

# Resolve THIS clone's repo root and launcher. Script lives in scripts/,
# launcher is at <repo>\symbion.bat next to symbion.ps1.
$RepoRoot    = Split-Path -Parent $PSScriptRoot
$BatLauncher = Join-Path $RepoRoot 'symbion.bat'
$Ps1Launcher = Join-Path $RepoRoot 'symbion.ps1'

if (-not (Test-Path -LiteralPath $BatLauncher)) {
    Write-Error "Launcher not found at $BatLauncher. Run install-cli.ps1 from inside a Symbion clone."
    exit 1
}

# Standard user-writable install location for personal CLIs. Off the main
# Programs tree so this can never collide with an MSI-installed app.
$ShimDir = Join-Path $env:LOCALAPPDATA 'Programs\symbion-cli'
$CmdShim = Join-Path $ShimDir 'symbion.cmd'

# --- Legacy cleanup: strip old profile-function block, if present --------
# Previous install-cli versions wrote a `function symbion { ... }` block
# into $PROFILE.CurrentUserAllHosts. Remove it on every run so the new
# .cmd shim is the single source of truth.
function Remove-LegacyProfileBlock {
    $profilePath = $PROFILE.CurrentUserAllHosts
    if (-not (Test-Path -LiteralPath $profilePath)) { return $false }
    $existing = Get-Content -LiteralPath $profilePath -Raw
    $pattern  = '(?ms)^\s*# >>> symbion-cli >>>.*?# <<< symbion-cli <<<\s*\r?\n?'
    $stripped = [regex]::Replace($existing, $pattern, '')
    if ($stripped -eq $existing) { return $false }
    if ([string]::IsNullOrWhiteSpace($stripped)) {
        Remove-Item -LiteralPath $profilePath -Force
    } else {
        Set-Content -LiteralPath $profilePath -Value $stripped -Encoding UTF8 -NoNewline
    }
    return $true
}

# --- PATH helpers --------------------------------------------------------
function Get-UserPathEntries {
    $raw = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ([string]::IsNullOrEmpty($raw)) { return @() }
    return ($raw -split ';') | Where-Object { $_ -ne '' }
}

function Set-UserPathEntries {
    param([string[]]$Entries)
    $value = ($Entries -join ';')
    [Environment]::SetEnvironmentVariable('Path', $value, 'User')
}

# --- Uninstall path ------------------------------------------------------
if ($Uninstall) {
    if (Test-Path -LiteralPath $ShimDir) {
        Remove-Item -LiteralPath $ShimDir -Recurse -Force
        Write-Host "Removed shim directory: $ShimDir"
    } else {
        Write-Host "Shim directory absent: $ShimDir"
    }

    $entries = Get-UserPathEntries
    if ($entries -contains $ShimDir) {
        Set-UserPathEntries -Entries ($entries | Where-Object { $_ -ne $ShimDir })
        Write-Host "Removed $ShimDir from User PATH"
    }

    if (Remove-LegacyProfileBlock) {
        Write-Host "Removed legacy symbion-cli block from PowerShell profile"
    }

    Write-Host ""
    Write-Host "Open a new shell for PATH change to take effect."
    exit 0
}

# --- Install path --------------------------------------------------------

# 1. Create the shim dir
if (-not (Test-Path -LiteralPath $ShimDir)) {
    New-Item -ItemType Directory -Path $ShimDir -Force | Out-Null
}

# 2. Write the .cmd shim. `call` so errorlevel propagates to the caller.
#    %* forwards all args verbatim. ASCII encoding so cmd parses cleanly
#    (UTF-8 BOM would be interpreted as a stray glyph in the first line).
$BatEscaped  = $BatLauncher.Replace('"', '""')
$cmdContent  = "@echo off`r`ncall `"$BatEscaped`" %*`r`n"
Set-Content -LiteralPath $CmdShim -Value $cmdContent -Encoding ASCII -NoNewline
Write-Host "Wrote shim: $CmdShim"
Write-Host "  -> $BatLauncher"

# 3. Add shim dir to User PATH if missing. Also patch the current session
#    so the user can run `symbion` without restarting their shell IF they
#    invoked install-cli directly (bootstrap-portable.bat spawns a fresh
#    powershell.exe so the current-session patch is moot there).
$entries = Get-UserPathEntries
if ($entries -notcontains $ShimDir) {
    Set-UserPathEntries -Entries (@($ShimDir) + $entries)
    $env:Path = "$ShimDir;$env:Path"
    Write-Host "Added $ShimDir to User PATH"
} else {
    Write-Host "User PATH already contains $ShimDir"
}

# 4. Relax CurrentUser ExecutionPolicy if it's Restricted/Undefined. The
#    .cmd shim doesn't need this -- it calls symbion.bat -- but anyone
#    invoking .\symbion.ps1 directly will hit the "cannot be loaded"
#    error without it. RemoteSigned is the standard relaxed level: local
#    scripts run, downloaded-with-MOTW scripts still require signing.
try {
    $currentPolicy = Get-ExecutionPolicy -Scope CurrentUser
    if ($currentPolicy -eq 'Restricted' -or $currentPolicy -eq 'Undefined') {
        Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force
        Write-Host "Set CurrentUser ExecutionPolicy: $currentPolicy -> RemoteSigned"
    } else {
        Write-Host "CurrentUser ExecutionPolicy already $currentPolicy (no change)"
    }
} catch {
    Write-Host "[warn] Could not set ExecutionPolicy: $($_.Exception.Message)"
    Write-Host "       The .cmd shim still works. .ps1 launchers may not."
}

# 5. Strip any legacy profile-function block from prior install-cli runs.
if (Remove-LegacyProfileBlock) {
    Write-Host "Cleaned up legacy profile-function block (now using .cmd shim)"
}

Write-Host ""
Write-Host "==========================================================="
Write-Host "Installed. Open a NEW shell (cmd OR PowerShell), then run:"
Write-Host "  symbion              (terminal REPL)"
Write-Host "  symbion --web        (web UI)"
Write-Host "==========================================================="
