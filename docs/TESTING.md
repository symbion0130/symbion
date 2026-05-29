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
```

Conversation routing smoke:

```powershell
.\scripts\smoke-conversation.ps1
```

The script posts isolated sessions to `/api/chat` and checks persona/routing regressions for social context, v14-style relational warmth, direct teaching, emotional mapping, crisis tone, MasterDocument-guided spiritual answers, and forget/wipe confirmation. It never confirms a full memory wipe.

Python tests were removed with the legacy runtime. Checks now target the native backend and WebView2 shell.
