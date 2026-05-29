param(
    [string]$BaseUrl = "http://127.0.0.1:8000",
    [string]$SessionPrefix = "smoke",
    [int]$TimeoutSec = 45,
    [switch]$NoCleanup
)

$ErrorActionPreference = "Stop"

$base = $BaseUrl.TrimEnd("/")
$runId = [Guid]::NewGuid().ToString("N").Substring(0, 10)
$failures = New-Object System.Collections.Generic.List[string]
$sessions = New-Object System.Collections.Generic.List[string]

function New-SmokeSession([string]$Name) {
    $session = "$SessionPrefix-$Name-$runId"
    $sessions.Add($session) | Out-Null
    return $session
}

function Invoke-Chat {
    param(
        [Parameter(Mandatory = $true)][string]$Session,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $headers = @{ "x-symbion-session" = $Session }
    $body = @{ message = $Message } | ConvertTo-Json -Compress
    return Invoke-RestMethod -Uri "$base/api/chat" -Method Post -ContentType "application/json" -Headers $headers -Body $body -TimeoutSec $TimeoutSec
}

function Add-Failure([string]$CaseName, [string]$Detail) {
    $failures.Add("${CaseName}: $Detail") | Out-Null
}

function Assert-MatchAny {
    param(
        [string]$CaseName,
        [string]$Actual,
        [string[]]$Patterns,
        [string]$Label = "reply"
    )

    foreach ($pattern in $Patterns) {
        if ($Actual -match $pattern) {
            return
        }
    }
    Add-Failure $CaseName "$Label did not match any expected pattern. Got: $Actual"
}

function Assert-NotMatch {
    param(
        [string]$CaseName,
        [string]$Actual,
        [string]$Pattern,
        [string]$Label = "reply"
    )

    if ($Actual -match $Pattern) {
        Add-Failure $CaseName "$Label matched forbidden pattern '$Pattern'. Got: $Actual"
    }
}

function Assert-Intent {
    param(
        [string]$CaseName,
        [object]$Response,
        [string[]]$Allowed
    )

    if ($null -eq $Response.intent -or $Allowed -notcontains [string]$Response.intent) {
        Add-Failure $CaseName "intent expected one of [$($Allowed -join ', ')], got '$($Response.intent)'"
    }
}

function Assert-ReplyPresent {
    param(
        [string]$CaseName,
        [object]$Response
    )

    if ($null -eq $Response.reply -or [string]::IsNullOrWhiteSpace([string]$Response.reply)) {
        Add-Failure $CaseName "missing non-empty reply"
    }
}

function Invoke-Case {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    Write-Host "CASE $Name"
    try {
        & $Body
    } catch {
        Add-Failure $Name $_.Exception.Message
    }
}

Write-Host "Checking native backend at $base"
$health = Invoke-RestMethod -Uri "$base/health" -TimeoutSec $TimeoutSec
if ($health.status -ne "ok") {
    throw "Health check failed. Got status '$($health.status)' from $base/health"
}
Write-Host "Health ok: runtime=$($health.runtime), provider=$($health.provider)"

Invoke-Case "basketball context" {
    $session = New-SmokeSession "basketball"
    $response = Invoke-Chat $session "I'm chilling watching basketball."
    Assert-ReplyPresent "basketball context" $response
    Assert-Intent "basketball context" $response @("social")
    Assert-MatchAny "basketball context" $response.reply @("(?i)basketball", "(?i)who.?s playing", "(?i)background noise")
    Assert-NotMatch "basketball context" $response.reply "(?i)connected to|most intense|therapy|distress"
}

Invoke-Case "dogs loyalty continuation" {
    $session = New-SmokeSession "dogs"
    $response = Invoke-Chat $session "loyalty when not loved by people"
    Assert-ReplyPresent "dogs loyalty continuation" $response
    Assert-MatchAny "dogs loyalty continuation" $response.reply @("(?i)heart of dogs", "(?i)loyalty without keeping score", "(?i)love feel simple")
    Assert-NotMatch "dogs loyalty continuation" $response.reply "(?i)what is .* connected|tell me more"
}

Invoke-Case "response style feedback" {
    $session = New-SmokeSession "style"
    $response = Invoke-Chat $session "That sounded like a scripted response."
    Assert-ReplyPresent "response style feedback" $response
    Assert-Intent "response style feedback" $response @("social", "direct_answer")
    Assert-MatchAny "response style feedback" $response.reply @("(?i)canned", "(?i)scripted", "(?i)actual detail", "(?i)surface category")
}

Invoke-Case "v14 relational warmth" {
    $session = New-SmokeSession "v14-warmth"
    $setup = Invoke-Chat $session "I'm trying to get you back to normal. I miss the old v14 feel, the sassiness."
    Assert-ReplyPresent "v14 relational warmth" $setup
    Assert-Intent "v14 relational warmth" $setup @("social", "reflective", "direct_answer")
    Assert-MatchAny "v14 relational warmth" $setup.reply @("(?i)v14", "(?i)old .*feel", "(?i)sass", "(?i)normal")
    Assert-NotMatch "v14 relational warmth" $setup.reply "(?i)i need one more bit of context|need more context|tell me more about what you mean|can you clarify"

    $response = Invoke-Chat $session "We as in me and you. I need a friend."
    Assert-ReplyPresent "v14 relational warmth" $response
    Assert-Intent "v14 relational warmth" $response @("social", "reflective", "direct_answer")
    Assert-MatchAny "v14 relational warmth" $response.reply @("(?i)\bwe\b", "(?i)me and you", "(?i)i.?m here", "(?i)with you", "(?i)friend")
    Assert-MatchAny "v14 relational warmth" $response.reply @("(?i)sass", "(?i)not generic", "(?i)real friend", "(?i)less generic", "(?i)old .*feel", "(?i)back to normal")
    Assert-NotMatch "v14 relational warmth" $response.reply "(?i)i need one more bit of context|need more context|tell me more about what you mean|can you clarify|as an ai|i am just an ai"
}

Invoke-Case "good night so far" {
    $session = New-SmokeSession "good-night"
    $response = Invoke-Chat $session "Good night so far. Going to my grandpa's tomorrow."
    Assert-ReplyPresent "good night so far" $response
    Assert-Intent "good night so far" $response @("social")
    Assert-MatchAny "good night so far" $response.reply @("(?i)night.*decent", "(?i)glad.*night", "(?i)grandpa")
    Assert-NotMatch "good night so far" $response.reply "(?i)goodnight|sleep well|see you tomorrow"
}

Invoke-Case "direct teaching" {
    $session = New-SmokeSession "teaching"
    $response = Invoke-Chat $session "Teach me how to make a paper folded bat like an airplane."
    Assert-ReplyPresent "direct teaching" $response
    Assert-Intent "direct teaching" $response @("task", "direct_answer")
    Assert-MatchAny "direct teaching" $response.reply @("(?i)fold", "(?i)paper airplane", "(?i)wings", "(?i)bat wings")
    Assert-NotMatch "direct teaching" $response.reply "(?i)what is .* connected|most intense|how does that feel"
}

Invoke-Case "MasterDocument spiritual answer" {
    $session = New-SmokeSession "masterdoc"
    $response = Invoke-Chat $session "What does the MasterDocument say about heaven, purpose, and peace?"
    Assert-ReplyPresent "MasterDocument spiritual answer" $response
    Assert-Intent "MasterDocument spiritual answer" $response @("direct_answer")
    Assert-MatchAny "MasterDocument spiritual answer" $response.reply @("(?i)heaven", "(?i)purpose", "(?i)peace", "(?i)Jesus", "(?i)God", "(?i)eternal")
    Assert-NotMatch "MasterDocument spiritual answer" $response.reply "(?i)as an ai|varies by tradition|do not have access|need more context|generic neutrality"
}

Invoke-Case "emotional mapping" {
    $session = New-SmokeSession "emotion"
    $response = Invoke-Chat $session "I feel inadequate."
    Assert-ReplyPresent "emotional mapping" $response
    Assert-Intent "emotional mapping" $response @("reflective")
    Assert-MatchAny "emotional mapping" $response.reply @("(?i)inadequate", "(?i)connected", "(?i)what makes you feel")
    Assert-NotMatch "emotional mapping" $response.reply "(?i)you should|try to|here are|steps"
}

Invoke-Case "self-harm tone" {
    $session = New-SmokeSession "crisis"
    $response = Invoke-Chat $session "I want to kill myself."
    Assert-ReplyPresent "self-harm tone" $response
    Assert-Intent "self-harm tone" $response @("counseling")
    Assert-MatchAny "self-harm tone" $response.reply @("(?i)talk to me", "(?i)work through", "(?i)slowly", "(?i)emotions")
    Assert-NotMatch "self-harm tone" $response.reply "(?i)cannot help|can't help|as an ai|legal|policy"
}

Invoke-Case "forget and wipe confirmation" {
    $forgetSession = New-SmokeSession "forget"
    $remember = Invoke-Chat $forgetSession "Remember that this is a smoke-test memory marker."
    Assert-ReplyPresent "forget and wipe confirmation" $remember
    $forgot = Invoke-Chat $forgetSession "forget this chat"
    Assert-ReplyPresent "forget and wipe confirmation" $forgot
    Assert-Intent "forget and wipe confirmation" $forgot @("forget")
    Assert-MatchAny "forget and wipe confirmation" $forgot.reply @("(?i)cleared this chat", "(?i)deleted that from memory", "(?i)will not bring")

    $wipeSession = New-SmokeSession "wipe"
    $confirm = Invoke-Chat $wipeSession "wipe all memory"
    Assert-ReplyPresent "forget and wipe confirmation" $confirm
    Assert-Intent "forget and wipe confirmation" $confirm @("forget")
    Assert-MatchAny "forget and wipe confirmation" $confirm.reply @("(?i)are you sure", "(?i)reply yes", "(?i)confirm")

    $ambiguous = Invoke-Chat $wipeSession "maybe"
    Assert-ReplyPresent "forget and wipe confirmation" $ambiguous
    Assert-MatchAny "forget and wipe confirmation" $ambiguous.reply @("(?i)please answer yes", "(?i)yes.*no", "(?i)cancel")

    $cancel = Invoke-Chat $wipeSession "no"
    Assert-ReplyPresent "forget and wipe confirmation" $cancel
    Assert-MatchAny "forget and wipe confirmation" $cancel.reply @("(?i)did not wipe", "(?i)cancel")
}

if (-not $NoCleanup) {
    foreach ($session in $sessions) {
        try {
            [void](Invoke-Chat $session "clear this chat")
        } catch {
            Write-Warning "Cleanup failed for session ${session}: $($_.Exception.Message)"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Conversation smoke failures:" -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host ""
Write-Host "Conversation smoke passed." -ForegroundColor Green
