# Symbion Native

This directory contains the active native Windows runtime for Symbion.

## Targets

- `symbion_native.exe`: WebView2 shell, tray, lifecycle, provider controls.
- `symbion_backend.exe`: C++ localhost backend with SQLite memory, emotional signal tracking, native tools, telemetry, and Local Gemma chat calls.

The shell launches the C++ backend directly. Electron and the tracked Python runtime are gone.

## Layout

- `src/`: C++ shell, backend, HTTP server, config, JSON helpers, Local Gemma client, intent routing, and memory store.
- `web/`: bundled WebView chat UI and assets served by the backend.
- `vendor/sqlite/`: SQLite amalgamation used by the backend.
- `CMakeLists.txt`: native build definition for the shell and backend targets.

## Backend Surface

The backend owns the runtime API surface:

- `/health`
- `/api/chat`
- `/api/local-gemma/status`
- `/api/messages/recent`
- `/api/emotions` and `/api/emotions/recent`
- `/api/sessions` and `/api/sessions/{id}/messages`
- `/api/profile/fact`
- `/api/memory/relevant`
- `/api/techniques` and `/api/techniques/sync`
- `/api/forget`

It stores local conversation state in SQLite, appends turn telemetry to JSONL,
retrieves relevant memory on demand, and routes fast deterministic requests
through native tool paths before falling back to Local Gemma.

## Build

```powershell
.\scripts\build-native.ps1 -WebView2SdkRoot "C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
```

## Run

```powershell
.\scripts\start-native.ps1
```

## Smoke

```powershell
.\native\build\Release\symbion_backend.exe --repo .
Invoke-RestMethod http://127.0.0.1:8000/health
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"I feel anxious"}'
```

For broader regression checks, see [`docs/TESTING.md`](../docs/TESTING.md).
