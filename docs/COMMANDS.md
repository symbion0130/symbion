# Symbion Commands

Symbion is a native C++ runtime. Electron and Python have been removed from the tracked app.

## Native Build

```powershell
.\scripts\build-native.ps1 -WebView2SdkRoot "C:\projects\symbion\native\.deps\Microsoft.Web.WebView2.1.0.3967.48"
```

## Native Runtime

```powershell
.\native\build\Release\symbion_backend.exe --repo .
.\native\build\Release\symbion_native.exe
```

Config lives at `config\symbion.json`. Runtime data lives under `data\`.

## Smoke

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"I feel anxious"}'
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"forget that memory"}'
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"what is 8472 multiplied by 91349 -- exact answer please"}'
Invoke-RestMethod http://127.0.0.1:8000/api/sessions
.\scripts\smoke-conversation.ps1
.\scripts\eval-native-v14.ps1 -Limit 40
```

## Package

```powershell
.\scripts\package-native.ps1
```
