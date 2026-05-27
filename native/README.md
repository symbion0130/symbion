# Symbion Native

This directory contains the native Windows runtime.

## Targets

- `symbion_native.exe`: WebView2 shell, tray, lifecycle, provider controls.
- `symbion_backend.exe`: C++ localhost backend with SQLite memory, emotional signal tracking, and Local Gemma chat calls.

The shell launches the C++ backend directly. Electron and the tracked Python runtime are gone.

## Build

```powershell
.\scripts\build-native.ps1 -WebView2SdkRoot "C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
```

## Run

```powershell
.\scripts\start-native.ps1
```

## Smoke

```powershell
.\native\build\Release\symbion_backend.exe --repo .
Invoke-RestMethod http://127.0.0.1:8000/health
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"I feel anxious"}'
```
