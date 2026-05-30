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

The script posts isolated sessions to `/api/chat` and checks persona/routing regressions for active-user memory scoping, warm casual presence, social context, low-signal social memory isolation, context-correction turns, v14-style relational warmth, restraint on tiny/direct prompts, sycophancy resistance, specialness/grandeur deflation, meta-identity honesty, identity/persona drift, calibration honesty, source/tool-output honesty, direct teaching, emotional mapping, crisis tone, MasterDocument-guided spiritual answers, canned support-bot phrasing, technique save/show/delete flows, and forget/wipe confirmation. It never confirms a full memory wipe.

The expanded v14 restraint smoke also checks direct date awareness against the current host year. On the current 2026-native runs, `what year is it` should answer `2026`; stale year answers indicate a native prompt/context issue rather than a smoke-script issue.

V14 golden-case regression smoke:

```powershell
.\scripts\eval-native-v14.ps1 -Limit 40
```

The native eval runner reads the old v14 JSONL golden files from `D:\symbion\evals`
when that drive is present. It checks restraint, banned canned phrases, required
answer fragments, max line/character budgets, and rough tool-path expectations
against native `/api/chat`. It is PowerShell-only and does not restore a Python
runtime dependency.

Native tool smoke should include exact math/date, local directory listing, URL fetch, weather lookup, and session message reload:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"what is 8472 multiplied by 91349 -- exact answer please"}'
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"list files in native/src"}'
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"fetch https://example.com"}'
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"weather in Tulsa OK"}'
```

Python tests were removed with the legacy runtime. Checks now target the native backend and WebView2 shell.
