# Symbion

Symbion is a local-first Windows AI assistant runtime. The current public
structure is a native C++ application: a WebView2 desktop shell, a C++
localhost backend, SQLite-backed memory, and Local Gemma through an
OpenAI-compatible `llama.cpp` server.

The tracked runtime no longer depends on Electron or Python. Earlier Python
versions informed the architecture, but the active app path is native C++.

## Project Status

Status: active native runtime, public documentation refreshed June 2026.

The public repository is meant to show the current app structure and the work
behind it: native Windows shell, C++ backend, SQLite memory, local model
integration, deterministic tools, and regression coverage. Local databases,
logs, packaged builds, private memories, and personal runtime paths are not
tracked.

## Current Architecture

- `symbion_native.exe`: Windows WebView2 shell with tray/lifecycle controls.
- `symbion_backend.exe`: C++ localhost backend for chat, memory, tools, and
  runtime APIs.
- `native/web/`: bundled HTML/CSS/JavaScript chat UI served by the backend.
- `native/vendor/sqlite/`: vendored SQLite amalgamation with FTS support.
- `config/`: runtime configuration.
- `data/`: local SQLite database and event telemetry, ignored by git.
- `docs/`: architecture notes, setup, testing, roadmap, and source material.
- `scripts/`: build, package, smoke-test, and helper scripts.

## Runtime Capabilities

- Local Gemma chat path using an OpenAI-compatible local server.
- SQLite message storage, session history, profile facts, and emotional signal
  tracking.
- Relevant memory retrieval with user scoping and same-session continuity.
- Native endpoints for chat, health, sessions, emotions, memory, techniques,
  and local model status.
- Deterministic tool paths for exact math/date/time, local file and directory
  reads, URL fetch, lightweight web search, weather lookup, and PDF text
  extraction.
- JSONL turn telemetry for latency, routing, memory counts, emotion signals,
  response source, and refresh/fallback behavior.
- Regression smoke scripts for routing, memory scoping, restraint, persona
  behavior, source honesty, tool handling, and native API health.

## Build

Requirements:

- Windows 10/11
- CMake
- Visual Studio 2022 Build Tools with the C++ workload
- Microsoft WebView2 Runtime
- Microsoft WebView2 SDK package extracted locally or supplied through
  `-WebView2SdkRoot`

Build the native binaries:

```powershell
.\scripts\build-native.ps1 -WebView2SdkRoot "C:\path\to\Microsoft.Web.WebView2"
```

## Run

```powershell
.\scripts\start-native.ps1
```

The shell starts the local backend and opens the bundled WebView UI. By
default, the backend listens on `127.0.0.1:8000`.

Useful smoke checks:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health
Invoke-RestMethod http://127.0.0.1:8000/api/local-gemma/status
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"I feel overwhelmed today"}'
```

## Package

```powershell
.\scripts\package-native.ps1
```

The package script stages the native binaries, config, web UI, and source text
needed by the runtime into `native/dist/`.

## Public Repo Notes

- `config/symbion.json` is a public-safe default config. Personal model paths,
  legacy memory imports, sync paths, databases, logs, and generated packages
  should stay local.
- No release artifacts are published yet. Build from source for now.
- License has not been selected yet; until a license is added, all rights are
  reserved by default under GitHub's normal public-repository behavior.
- Good GitHub topics for this repo: `ai-assistant`, `cpp`, `webview2`,
  `sqlite`, `local-first`, `llama-cpp`, `gemma`, `windows`.

## Documentation

- [Setup](docs/SETUP.md)
- [Testing](docs/TESTING.md)
- [Memory architecture](docs/MEMORY_ARCHITECTURE.md)
- [Local Gemma runtime](docs/LOCAL_GEMMA.md)
- [Native migration notes](docs/WEBVIEW2_MIGRATION.md)
- [Roadmap](docs/ROADMAP.md)
