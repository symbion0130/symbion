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
- `symbion_backend.exe` builds and serves `/health`, `/api/local-gemma/status`, `/api/chat`, `/api/messages/recent`, `/api/emotions/recent`, and the native web UI.
- The shell launches `symbion_backend.exe` directly.
- SQLite message storage and emotional signal tracking are native.
- Local Gemma chat calls are native.

## Remaining Migration Work

- Expand SQLite schema compatibility with old memory tables where useful.
- Improve memory ranking beyond the current lightweight keyword retrieval.
- Add streaming responses.
- Add native tests around memory, chat, and prompt behavior.
- Rebuild native test coverage around the C++ backend.
