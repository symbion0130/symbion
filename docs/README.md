# Symbion

Symbion is a lightweight native Windows runtime:

- C++ WebView2 shell
- C++ local backend with SQLite memory
- Local Gemma by default through the OpenAI-compatible llama.cpp server

Electron and Python have been removed from the tracked runtime. The native backend now owns health, chat, local Gemma calls, SQLite message storage, and emotional signal tracking.

## Build

```powershell
.\scripts\build-native.ps1 -WebView2SdkRoot "C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
```

## Run

```powershell
.\scripts\start-native.ps1
```

## Package

```powershell
.\scripts\package-native.ps1
```

See [docs/WEBVIEW2_MIGRATION.md](docs/WEBVIEW2_MIGRATION.md) for the native migration status.
