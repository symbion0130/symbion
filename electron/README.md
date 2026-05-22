# Symbion Electron app

Native desktop shell for Symbion. Wraps the existing web UI in a
proper window so you don't have to keep a browser tab pinned.

## Architecture

```
┌─ Electron main process ─────────────────────┐
│                                              │
│   spawn:   ../.python/python.exe \           │
│              -m symbion --web                 │
│   poll /health until 200                      │
│   open BrowserWindow → http://127.0.0.1:8000  │
│   on quit: taskkill /T /F <backend pid>       │
│                                              │
└──────────────────────────────────────────────┘
```

No code changes to Symbion itself. The Electron process is a thin shell
that owns the lifecycle of the Python backend and the window that
renders the existing FastAPI UI.

## Dev run

```powershell
cd electron
npm install            # one time; pulls electron + electron-builder (~150MB)
npm start              # launches the window
```

First run takes 10-30s while Symbion's backend boots (the model loaders
and SQLite migrations run before /health returns 200). Subsequent
launches are faster — Symbion's startup is mostly steady-state.

## Packaged installer

```powershell
npm run build:win      # produces dist\Symbion Setup <version>.exe
```

Builds an NSIS installer that drops the Electron app into
`%LOCALAPPDATA%\Programs\Symbion`. It does NOT bundle the Python backend
— the installer assumes Symbion's repo is already at
`%USERPROFILE%\symbion` (via the standard `install.ps1` flow). If you
want a single-file install that includes Python + models, that's a
bigger packaging job (~5GB output with everything bundled).

## Single-instance lock

Double-clicking the shortcut after the app is already running just
focuses the existing window — it doesn't spawn a second backend (which
would fail to bind to port 8000 and look like Symbion is broken).

## Config

- `web_port` is read from `../symbion.json` so changes there flow
  through automatically.
- The Python interpreter is picked in this order:
  1. `../.python/python.exe` (portable Python from bootstrap-portable.bat)
  2. `python` on PATH
