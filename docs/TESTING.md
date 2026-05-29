# Testing

## Native

```powershell
cmake --build native\build --config Release
.\native\build\Release\symbion_backend.exe --repo .
```

Expected smoke endpoint:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health
```

Chat smoke:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"I feel overwhelmed today"}'
Invoke-RestMethod http://127.0.0.1:8000/api/messages/recent
Invoke-RestMethod http://127.0.0.1:8000/api/emotions/recent
Invoke-RestMethod http://127.0.0.1:8000/api/sessions
```

Conversation routing smoke:

```powershell
.\scripts\smoke-conversation.ps1
```

<<<<<<<<< Temporary merge branch 1
The script posts isolated sessions to `/api/chat` and checks persona/routing regressions for social context, v14-style relational warmth, direct teaching, emotional mapping, crisis tone, MasterDocument-guided spiritual answers, and forget/wipe confirmation. It never confirms a full memory wipe.
=========
The script posts isolated sessions to `/api/chat` and checks persona/routing regressions for social context, v14-style relational warmth, restraint on tiny/direct prompts, sycophancy resistance, specialness/grandeur deflation, meta-identity honesty, identity/persona drift, calibration honesty, source/tool-output honesty, direct teaching, emotional mapping, crisis tone, MasterDocument-guided spiritual answers, canned support-bot phrasing, technique save/show/delete flows, and forget/wipe confirmation. It never confirms a full memory wipe.

The expanded v14 restraint smoke also checks direct date awareness against the current host year. On the current 2026-native runs, `what year is it` should answer `2026`; stale year answers indicate a native prompt/context issue rather than a smoke-script issue.
>>>>>>>>> Temporary merge branch 2

Python tests were removed with the legacy runtime. Checks now target the native backend and WebView2 shell.
