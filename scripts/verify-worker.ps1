<#
.SYNOPSIS
    Smoke-test the Symbion install Worker.

.DESCRIPTION
    Confirms behaviours of the Cloudflare Worker that serves install.ps1:

      1. No token            -> 403
      2. Wrong token         -> 403
      3. Correct token       -> 200
      4. Served script has all six Worker-injected placeholders
         substituted, and each substituted value is non-empty:
           - __SYMBION_PAT_INJECTED__              (+ github_pat_ prefix check)
           - __SYMBION_ANTHROPIC_KEY_INJECTED__    (+ sk-ant-api03- prefix check)
           - __SYMBION_BRAVE_KEY_INJECTED__
           - __SYMBION_API_KEY_INJECTED__
           - __SYMBION_GROQ_KEY_INJECTED__
           - __SYMBION_KIMI_KEY_INJECTED__

    Never prints the token, the served script body, or the substituted
    secrets. Probes are substring/regex matches that only emit booleans
    and value lengths.

.PARAMETER WorkerUrl
    Base URL of the Worker. Default:
    https://symbion-installer.symbion-0130.workers.dev

.PARAMETER Token
    Install token. Falls back to $env:SYMBION_INSTALL_TOKEN.

.EXAMPLE
    $env:SYMBION_INSTALL_TOKEN = '<your-token>'
    .\scripts\verify-worker.ps1

.EXAMPLE
    .\scripts\verify-worker.ps1 -Token '<your-token>' -WorkerUrl 'https://staging-worker.example.workers.dev'
#>
[CmdletBinding()]
param(
    [string]$WorkerUrl = 'https://symbion-installer.symbion-0130.workers.dev',
    [string]$Token
)

$ErrorActionPreference = 'Stop'

if (-not $Token) { $Token = $env:SYMBION_INSTALL_TOKEN }
if (-not $Token) {
    Write-Host "[error] No token provided. Pass -Token or set `$env:SYMBION_INSTALL_TOKEN." -ForegroundColor Red
    exit 2
}

# Force TLS 1.2 for PS 5.1 environments that still negotiate TLS 1.0 by
# default; Cloudflare requires 1.2+.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$script:results = @()

function Add-Result($name, $pass, $detail) {
    $script:results += [pscustomobject]@{
        Check  = $name
        Pass   = [bool]$pass
        Detail = $detail
    }
}

# Invoke-WebRequest throws on non-2xx with $ErrorActionPreference=Stop.
# This wrapper returns the status code regardless of class so the 403
# checks can assert on the status rather than parse exception messages.
# Handles both the PS 5.1 (System.Net.WebException) and PS 7+
# (Microsoft.PowerShell.Commands.HttpResponseException) exception types
# by reading $_.Exception.Response.StatusCode in both cases.
function Get-StatusCode {
    param([string]$Url, [hashtable]$Headers = @{})
    try {
        $r = Invoke-WebRequest -Uri $Url -Headers $Headers -UseBasicParsing -Method GET -ErrorAction Stop
        return [int]$r.StatusCode
    } catch {
        if ($_.Exception.Response) {
            return [int]$_.Exception.Response.StatusCode
        }
        throw
    }
}

Write-Host ''
Write-Host "Verifying $WorkerUrl" -ForegroundColor Cyan
Write-Host ''

# --- 1. No token -> 403 -------------------------------------------------
try {
    $code = Get-StatusCode -Url $WorkerUrl
    Add-Result 'no-token-403' ($code -eq 403) "status=$code (expected 403)"
} catch {
    Add-Result 'no-token-403' $false "network/exception: $($_.Exception.Message)"
}

# --- 2. Wrong token -> 403 ----------------------------------------------
# Token is sent via the `?t=<token>` query param -- the same path
# install.ps1's `irm` one-liner uses. The Worker doesn't accept the
# Authorization header; that was a verify-script-only convention.
try {
    $code = Get-StatusCode -Url ("$WorkerUrl" + "?t=definitely-not-the-real-token")
    Add-Result 'wrong-token-403' ($code -eq 403) "status=$code (expected 403)"
} catch {
    Add-Result 'wrong-token-403' $false "network/exception: $($_.Exception.Message)"
}

# --- 3. Correct token -> 200, plus served-script content checks ---------
try {
    $resp = Invoke-WebRequest -Uri ("$WorkerUrl" + "?t=$Token") `
        -UseBasicParsing -Method GET -ErrorAction Stop
    $status = [int]$resp.StatusCode
    $body   = $resp.Content

    Add-Result 'good-token-200' ($status -eq 200) "status=$status (expected 200)"

    # We deliberately probe for substring presence rather than echoing the
    # script body. Each result is a boolean only; secrets never reach the
    # terminal.
    $hasPatPlaceholder    = $body.Contains('__SYMBION_PAT_INJECTED__')
    Add-Result 'pat-placeholder-substituted' (-not $hasPatPlaceholder) `
        $(if ($hasPatPlaceholder) { 'PAT placeholder STILL present in served script' } else { 'placeholder absent' })

    $hasKeyPlaceholder    = $body.Contains('__SYMBION_ANTHROPIC_KEY_INJECTED__')
    Add-Result 'anthropic-placeholder-substituted' (-not $hasKeyPlaceholder) `
        $(if ($hasKeyPlaceholder) { 'Anthropic placeholder STILL present in served script' } else { 'placeholder absent' })

    $hasAnthropicKey = $body -match 'sk-ant-api03-'
    Add-Result 'anthropic-key-injected' $hasAnthropicKey `
        $(if ($hasAnthropicKey) { 'sk-ant-api03- prefix found' } else { 'no Anthropic key prefix in served script' })

    # GitHub fine-grained PATs start with 'github_pat_'; classic tokens
    # start with 'ghp_'. Accept either so a future PAT-format change
    # doesn't fail the check spuriously.
    $hasGhPat = ($body -match 'github_pat_') -or ($body -match 'ghp_')
    Add-Result 'gh-pat-injected' $hasGhPat `
        $(if ($hasGhPat) { 'GitHub PAT prefix found' } else { 'no GitHub PAT prefix in served script' })

    # Loop through the two additional injected provider keys (Brave +
    # Symbion-internal). For each:
    #   (a) the placeholder string must be absent from the served body;
    #   (b) the value substituted into the Resolve-InjectedKey call must
    #       be non-empty (catches the case where the Worker secret is
    #       unset and the `|| ''` fallback wrote an empty string).
    # No format/prefix checks: Brave and Symbion don't have a universal
    # canonical prefix that's safe to grep for.
    $extraKeys = [ordered]@{
        'brave'   = @{ placeholder = '__SYMBION_BRAVE_KEY_INJECTED__'; envName = 'BRAVE_API_KEY'   }
        'symbion' = @{ placeholder = '__SYMBION_API_KEY_INJECTED__';   envName = 'SYMBION_API_KEY' }
        'groq'    = @{ placeholder = '__SYMBION_GROQ_KEY_INJECTED__';  envName = 'GROQ_API_KEY'    }
        'kimi'    = @{ placeholder = '__SYMBION_KIMI_KEY_INJECTED__';  envName = 'KIMI_API_KEY'    }
    }
    foreach ($entry in $extraKeys.GetEnumerator()) {
        $shortname   = $entry.Key
        $placeholder = $entry.Value.placeholder
        $envVar      = $entry.Value.envName

        $stillThere = $body.Contains($placeholder)
        Add-Result "$shortname-placeholder-substituted" (-not $stillThere) `
            $(if ($stillThere) { 'placeholder STILL present in served script' } else { 'placeholder absent' })

        # Match the Resolve-InjectedKey call for this env name and
        # capture the substituted Placeholder argument value.
        $pattern = "-Placeholder\s+'([^']*)'\s+-EnvVarName\s+'$([regex]::Escape($envVar))'"
        if ($body -match $pattern) {
            $value = $Matches[1]
            # "Injected" means: non-empty AND not the literal placeholder.
            # The literal-placeholder check catches the case where the
            # Worker hasn't been updated yet to substitute this key.
            $injected = (-not [string]::IsNullOrWhiteSpace($value)) -and (-not $value.StartsWith('__SYMBION'))
            $detail = if ($injected) {
                "non-empty value substituted (len=$($value.Length))"
            } elseif ($value.StartsWith('__SYMBION')) {
                'value is still the literal placeholder (Worker not updated?)'
            } else {
                'empty value (Worker secret missing?)'
            }
            Add-Result "$shortname-key-injected" $injected $detail
        } else {
            Add-Result "$shortname-key-injected" $false "could not locate Resolve-InjectedKey call for $envVar"
        }
    }
} catch {
    Add-Result 'good-token-200' $false "network/exception: $($_.Exception.Message)"
}

# --- Report -------------------------------------------------------------
Write-Host ''
$results | ForEach-Object {
    $mark  = if ($_.Pass) { '[ OK ]' } else { '[FAIL]' }
    $color = if ($_.Pass) { 'Green' } else { 'Red' }
    Write-Host ("{0} {1,-38} {2}" -f $mark, $_.Check, $_.Detail) -ForegroundColor $color
}

$failed = ($results | Where-Object { -not $_.Pass } | Measure-Object).Count
Write-Host ''
if ($failed -eq 0) {
    Write-Host 'All checks passed.' -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failed check(s) failed." -ForegroundColor Red
    exit 1
}
