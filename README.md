# Symbion

Symbion is being moved to a lightweight native Windows runtime:

- C++ WebView2 shell
- C++ local backend
- SQLite memory
- Local Gemma by default

Electron and the tracked Python runtime have been removed. The native backend is intentionally small right now; memory, chat, and counseling features are being ported into C++ next.

## Build

```powershell
cmake -S native -B native\build -G "Visual Studio 17 2022" -A x64 -DSYMBION_WEBVIEW2_SDK_ROOT="C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
cmake --build native\build --config Release
```

## Run

```powershell
.\scripts\start-native.ps1
```

See [docs/WEBVIEW2_MIGRATION.md](docs/WEBVIEW2_MIGRATION.md) for the native migration status.
