# Symbion Commands

Symbion is being moved to a native C++ runtime. Electron has been removed.

## Native Build

```powershell
cmake -S native -B native\build -G "Visual Studio 17 2022" -A x64 -DSYMBION_WEBVIEW2_SDK_ROOT="C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
cmake --build native\build --config Release
.\scripts\start-native.ps1
```

## Native Runtime

```powershell
.\native\build\Release\symbion_backend.exe --repo .
.\native\build\Release\symbion_native.exe
```

The native shell now launches `symbion_backend.exe`, not Electron and not Python.
