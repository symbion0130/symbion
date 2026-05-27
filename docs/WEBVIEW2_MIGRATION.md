# Native Runtime Migration

Electron has been removed from the repo.

The current target is:

- C++ WebView2 desktop shell.
- C++ local backend process.
- SQLite-backed memory.
- Local Gemma as the default LLM provider.
- No Python in the production runtime.

## Current Native Status

- `symbion_native.exe` builds and hosts WebView2.
- `symbion_backend.exe` builds and serves `/health`, `/api/local-gemma/status`, and the existing web template.
- The shell launches `symbion_backend.exe` directly.

## Remaining Migration Work

- Port SQLite schema management and migrations.
- Port memory retrieval and emotional check-in storage.
- Port prompt assembly and local Gemma chat calls.
- Replace the old web API endpoints used by the current UI.
- Rebuild native test coverage around the C++ backend.
