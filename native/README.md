# Symbion Native Shell

CMake/C++ scaffold for the planned Windows WebView2 host.

The native shell is still a host around the existing Symbion FastAPI web UI. It preserves the current web interface, but now owns the desktop lifecycle pieces needed to move away from Electron without changing Python backend behavior.

## Current Scope

- Creates a Win32 desktop window titled `Symbion`.
- Loads `http://127.0.0.1:8000/` when WebView2 SDK headers and `WebView2Loader.dll` are available.
- Shows a clear placeholder window when WebView2 is not available at build/run time.
- Allows the target URL to be overridden with `SYMBION_WEBVIEW2_URL`, `--url <url>`, or `--url=<url>`.
- Enforces a single native app instance and focuses the existing window on relaunch.
- Adds tray show/hide/quit behavior with backend/provider/Gemma status.
- Starts `python -u -m symbion --web --host 127.0.0.1` when no backend is already serving `/health`.
- Attaches to an already-running backend instead of spawning a duplicate.
- Stops the owned backend process tree on quit.
- Adds provider switching by updating `symbion.json` and restarting the owned backend.
- Adds Local Gemma start/status/stop hooks.
- Reads local API keys from `symbion.json` `api_key` or `.env` `SYMBION_API_KEY` and injects `X-API-Key` into WebView2 requests for the local backend when available.

## Runtime Behavior

Repo discovery follows the Electron shell's shape:

1. `SYMBION_REPO`
2. Current working directory
3. Parent directories of the native executable
4. `%USERPROFILE%\symbion`
5. `%USERPROFILE%\SourceCode\symbion`

Python discovery prefers `<repo>\.python\python.exe`, then falls back to `python` on `PATH`.

The shell reads `symbion.json` for:

- `web_port`
- `llm_provider`
- `local_gemma_base_url`
- `local_gemma_start_script`
- optional `local_gemma_stop_command`
- optional `native_tray_enabled`
- optional `api_key`

`electron_tray_enabled` is honored as a compatibility fallback until native-specific config is settled.

Local Gemma stop is intentionally a hook: if `local_gemma_stop_command` or `SYMBION_GEMMA_STOP_COMMAND` is configured, the native shell runs it. Otherwise it can only stop a still-live process that it directly launched.

## Configure

```powershell
cmake -S native -B native\build
cmake --build native\build
```

To enable the real WebView2 host path, install or unpack the `Microsoft.Web.WebView2` SDK package and point CMake at the package root:

```powershell
cmake -S native -B native\build -DSYMBION_WEBVIEW2_SDK_ROOT=C:\path\to\Microsoft.Web.WebView2
```

The CMake file copies `WebView2Loader.dll` beside the executable when it finds the standard NuGet package layout.

## Verified Build

This shell has been configured and built with CMake + Visual Studio Build Tools 2022.

```powershell
cmake -S native -B native\build -G "Visual Studio 17 2022" -A x64 -DSYMBION_WEBVIEW2_SDK_ROOT=C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48
cmake --build native\build --config Release --target symbion_native
```

Current package artifact:

```text
native\dist\symbion-native-webview2-release.zip
```

From the repo root, launch the built native shell with:

```powershell
.\scripts\start-native.ps1
```

See [../docs/WEBVIEW2_MIGRATION.md](../docs/WEBVIEW2_MIGRATION.md) for the phased migration plan.
