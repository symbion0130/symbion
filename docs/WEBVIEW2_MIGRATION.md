# WebView2 Migration

The current desktop shell is Electron. A minimal native scaffold now exists under `native/`, so WebView2 work should be treated as a staged migration rather than a replacement already underway.

## Proposed Phases

Phase 1 keeps the existing Python/FastAPI backend and builds a minimal Windows WebView2 host around the local web UI.

The native scaffold now covers the main shell lifecycle:

- It owns a Win32 window and attempts to host WebView2.
- It targets `http://127.0.0.1:8000/` by default.
- It can be pointed at another local URL with `SYMBION_WEBVIEW2_URL` or `--url`.
- It enforces a single app instance.
- It provides tray show/hide/quit behavior.
- It starts `python -u -m symbion --web --host 127.0.0.1` when `/health` is not already reachable, so native-owned backend startup remains localhost-only unless the user launches a LAN backend separately.
- It attaches to an already-running backend rather than spawning a duplicate.
- It stops the owned backend process tree on quit.
- It exposes provider switching through the native menu/tray by updating `symbion.json`.
- It exposes Local Gemma start/status/stop hooks.
- It reads local API key material from `symbion.json` or `.env` and injects `X-API-Key` for WebView2 requests to the local backend when available.

Phase 2 should harden native packaging, update flow, icons/assets, installer behavior, and any missing analytics/status views.

Phase 3 replaces Python backend modules only where packaging, startup time, or performance justifies the extra native complexity.

## Parity Checklist

Before deprecating Electron, the WebView2 shell should match these user-visible behaviors:

- [x] Launch and stop the owned backend cleanly.
- [x] Attach to an externally launched backend.
- [x] Load the local chat UI.
- [x] Provide tray show/hide/quit behavior.
- [x] Enforce a single app instance.
- [x] Surface provider and local-runtime status.
- [x] Preserve local auth/key handling scaffold for protected local routes.
- [x] Define native update flow: ship native releases through the installer/package channel; the shell itself does not run Electron's updater.
- [ ] Package native app with WebView2 loader/runtime assumptions documented.
- [x] Add native icon fallback path. The scaffold uses the Windows application icon until final branded native resources are packaged.
- [x] Add analytics/status navigation parity through the existing in-web analytics/status surfaces.
- [x] Shut down without orphaning the owned backend.
- [x] Define Local Gemma stop ownership: the native shell owns processes it launched directly; externally launched Gemma is stopped only through `local_gemma_stop_command` or `SYMBION_GEMMA_STOP_COMMAND`.

## Delivery Choice

The lowest-risk bridge is to serve the existing web UI from the Python backend during Phase 1. Bundling static resources into the native app can wait until the shell is stable and the backend boundary is clearer.

## Native Config Hooks

The native shell currently reads the existing `symbion.json` plus a few migration-friendly optional keys:

- `native_tray_enabled`: overrides tray behavior for the native shell.
- `local_gemma_stop_command`: command used by the native shell's "Stop Local Gemma" action.
- `api_key`: used for `X-API-Key` injection into WebView2 requests.

If `api_key` is absent, `.env` `SYMBION_API_KEY` is used. Avoid putting cloud provider keys in native-specific files; cloud keys should stay in `.env` and remain backend-owned.

## Verified Native Build

Verified on May 27, 2026:

- Installed CMake with `winget`.
- Installed Visual Studio Build Tools 2022 with the MSVC C++ workload.
- Downloaded Microsoft.Web.WebView2 SDK `1.0.3967.48` into `native/.deps/`.
- Configured with CMake using the Visual Studio 17 2022 generator and x64 platform.
- Built `native/build/Release/symbion_native.exe`.
- Confirmed `WebView2Loader.dll` is copied beside the executable.
- Packaged `native/dist/symbion-native-webview2-release.zip`.
- Smoked native startup: the shell starts `python -u -m symbion --web --host 127.0.0.1`, `/health` reports `provider=local_gemma`, and the WebView2 process attaches.
- Smoked native Quit command: the shell exits, its owned backend exits, and `127.0.0.1:8000` becomes unreachable.
