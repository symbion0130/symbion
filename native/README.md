# Symbion Native

This directory contains the native Windows runtime.

## Targets

- `symbion_native.exe`: WebView2 shell, tray, lifecycle, provider controls.
- `symbion_backend.exe`: lightweight C++ localhost backend.

The shell launches the C++ backend directly. Electron and the tracked Python runtime are gone.

## Build

```powershell
cmake -S native -B native\build -G "Visual Studio 17 2022" -A x64 -DSYMBION_WEBVIEW2_SDK_ROOT="C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
cmake --build native\build --config Release
```

## Run

```powershell
.\scripts\start-native.ps1
```

## Smoke

```powershell
.\native\build\Release\symbion_backend.exe --repo .
Invoke-RestMethod http://127.0.0.1:8000/health
```
