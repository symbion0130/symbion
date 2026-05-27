# Setup

Symbion is now a Windows native WebView2 application with a C++ backend.

## Requirements

- Windows 10/11
- CMake
- Visual Studio 2022 Build Tools with C++ workload
- Microsoft WebView2 Runtime
- WebView2 SDK extracted under `native\.deps\...` or passed to `scripts\build-native.ps1`

## Build

```powershell
.\scripts\build-native.ps1 -WebView2SdkRoot "C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
```

## Run

```powershell
.\scripts\start-native.ps1
```

The shell starts the local native backend on `127.0.0.1`.

## Project Layout

- `config/` stores app configuration.
- `data/` stores local SQLite runtime data.
- `docs/source/` stores source documents.
- `logs/` stores local runtime logs.
