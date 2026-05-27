# Testing

## Native

```powershell
cmake --build native\build --config Release
.\native\build\Release\symbion_backend.exe --repo .
```

Expected smoke endpoint:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health
```

Python tests were removed with the legacy runtime. New checks should target the native backend and WebView2 shell.
