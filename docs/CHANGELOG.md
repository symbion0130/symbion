# Changelog

## 2026-05-27 Native C++ Runtime Pivot

- Removed Electron source, package metadata, assets, and installer script.
- Removed the tracked Python runtime, launchers, package metadata, tests, eval harness, and helper scripts.
- Added `symbion_backend.exe`, a lightweight C++ localhost backend scaffold.
- Updated the WebView2 shell to launch the C++ backend directly.
- Kept SQLite data files local and untouched.
