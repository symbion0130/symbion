# Setup

Symbion is now a Windows native WebView2 application with a C++ backend migration underway.

## Requirements

- Windows 10/11
- CMake
- Visual Studio 2022 Build Tools with C++ workload
- Microsoft WebView2 Runtime
- WebView2 SDK extracted under `native\.deps\...` or passed with `SYMBION_WEBVIEW2_SDK_ROOT`

## Build

```powershell
cmake -S native -B native\build -G "Visual Studio 17 2022" -A x64 -DSYMBION_WEBVIEW2_SDK_ROOT="C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
cmake --build native\build --config Release
```

## Run

```powershell
.\scripts\start-native.ps1
```

The shell starts the local native backend on `127.0.0.1`.
