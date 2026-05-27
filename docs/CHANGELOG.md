# Changelog

## 2026-05-27 Native C++ Runtime Pivot

- Removed Electron source, package metadata, assets, and installer script.
- Removed the tracked Python runtime, launchers, package metadata, tests, eval harness, and helper scripts.
- Added `symbion_backend.exe`, a lightweight C++ localhost backend scaffold.
- Updated the WebView2 shell to launch the C++ backend directly.
- Kept SQLite data files local and untouched.
- Added native SQLite message storage and emotional signal tracking.
- Added native Local Gemma chat calls and `/api/chat`.
- Replaced the placeholder web page with a native chat UI.
