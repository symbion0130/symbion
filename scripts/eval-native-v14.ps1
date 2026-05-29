param(
    [string]$BaseUrl = "http://127.0.0.1:8000",
    [string]$GoldenRoot = "D:\symbion\evals",
    [string[]]$GoldenFiles = @("golden_restraint.jsonl", "golden_tool_judgment.jsonl", "golden.jsonl"),
    [int]$Limit = 0,
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$eventsPath = Join-Path $repo "data\symbion_events.jsonl"
$runId = "eval-v14-" + (Get-Date -Format "yyyyMMdd-HHmmss")
$base = $BaseUrl.TrimEnd("/")

function Read-GoldenCases {
    $cases = @()
    foreach ($name in $GoldenFiles) {
        $path = Join-Path $GoldenRoot $name
        if (!(Test-Path $path)) {
            Write-Warning "Skipping missing golden file: $path"
            continue
        }
        Get-Content $path | ForEach-Object {
            $line = $_.Trim()
            if (!$line) { return }
            $case = $line | ConvertFrom-Json
            $case | Add-Member -NotePropertyName golden_file -NotePropertyValue $name -Force
            $cases += $case
        }
    }
    if ($Limit -gt 0) { return $cases | Select-Object -First $Limit }
    return $cases
}

function Invoke-NativeChat($session, $message) {
    $body = @{ message = $message } | ConvertTo-Json -Compress
    return Invoke-RestMethod -Uri "$base/api/chat" `
        -Method Post `
        -ContentType "application/json" `
        -Headers @{ "X-Symbion-Session" = $session } `
        -Body $body `
        -TimeoutSec $TimeoutSec
}

function Get-LatestTurnEvent($session) {
    if (!(Test-Path $eventsPath)) { return $null }
    $matches = Get-Content $eventsPath -Tail 200 | ForEach-Object {
        try { $_ | ConvertFrom-Json } catch { $null }
    } | Where-Object { $_ -and $_.event -eq "turn" -and $_.session -eq $session }
    return $matches | Select-Object -Last 1
}

function Test-Contains($text, $needle) {
    if ($null -eq $needle -or "$needle" -eq "") { return $true }
    return $text -match [Regex]::Escape([string]$needle)
}

$cases = @(Read-GoldenCases)
if ($cases.Count -eq 0) {
    throw "No golden cases found under $GoldenRoot"
}

Write-Host "Running $($cases.Count) v14-native eval cases against $base"
$failures = @()
$idx = 0
foreach ($case in $cases) {
    $idx++
    $id = if ($case.id) { $case.id } else { "case_$idx" }
    $session = "$runId-$id"
    try {
        $resp = Invoke-NativeChat $session $case.query
        $reply = [string]$resp.reply
        if ([string]::IsNullOrWhiteSpace($reply)) {
            $failures += "$id empty reply"
            continue
        }
        foreach ($needle in @($case.must_include)) {
            if (!(Test-Contains $reply $needle)) {
                $failures += "$id missing required text '$needle' :: $($reply.Substring(0, [Math]::Min(180, $reply.Length)))"
            }
        }
        foreach ($needle in @($case.must_not_include)) {
            if ($needle -and $reply -match [Regex]::Escape([string]$needle)) {
                $failures += "$id included banned text '$needle' :: $($reply.Substring(0, [Math]::Min(180, $reply.Length)))"
            }
        }
        if ($case.max_chars -and $reply.Length -gt [int]$case.max_chars) {
            $failures += "$id too long: $($reply.Length) > $($case.max_chars)"
        }
        if ($case.max_lines) {
            $lineCount = @($reply -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 }).Count
            if ($lineCount -gt [int]$case.max_lines) {
                $failures += "$id too many nonblank lines: $lineCount > $($case.max_lines)"
            }
        }

        $event = Get-LatestTurnEvent $session
        if ($event) {
            if ($case.max_tool_calls -eq 0 -and $event.response_source -eq "native_tool") {
                $failures += "$id over-tooled: response_source=native_tool"
            }
            if ($case.must_call_tools -and @($case.must_call_tools).Count -gt 0 -and $event.response_source -ne "native_tool") {
                $failures += "$id expected native tool path for $($case.must_call_tools -join ',') but got $($event.response_source)"
            }
        }
        Write-Host ("[{0}/{1}] {2} ok ({3})" -f $idx, $cases.Count, $id, $resp.intent)
    } catch {
        $failures += "$id exception: $($_.Exception.Message)"
    }
}

if ($failures.Count -gt 0) {
    Write-Host "`nNative v14 eval failures:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host " - $_" -ForegroundColor Red }
    exit 1
}

Write-Host "Native v14 eval passed." -ForegroundColor Green
