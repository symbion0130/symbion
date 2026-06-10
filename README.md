# Symbion

Symbion is a local-first Windows AI assistant runtime built as a native C++
desktop application. It combines a WebView2 shell, a C++ localhost backend,
SQLite memory, deterministic tool routes, and a local Gemma model served through
an OpenAI-compatible `llama.cpp` endpoint.

The active tracked runtime is native C++. Electron and the legacy Python app
were removed from the public runtime path after informing the architecture.

## What This Repo Demonstrates

This repository shows a full working AI product stack, not just a prompt or a
chat wrapper:

- Native Windows shell with WebView2, tray lifecycle, provider controls, app
  icon integration, backend ownership, and self-healing backend startup.
- C++ backend serving chat, memory, session, emotion, technique, local-model,
  forget/reset, static UI, and health endpoints.
- Local-first memory system on SQLite with user-scoped messages, sessions,
  profile facts, emotion signals, relevant-memory retrieval, summaries, and
  shared technique sync.
- Local Gemma integration through an OpenAI-compatible `llama.cpp` server,
  including prompt construction, mode-specific behavior, local retry/repair
  paths, and session summarization.
- Deterministic native tools for exact math, date/time, file and directory
  reads, URL fetches, lightweight web search, weather lookup, and text-based
  PDF extraction.
- Behavioral smoke coverage for routing, memory isolation, task mode,
  reflective/counseling behavior, source honesty, direct answers, tools,
  crisis handling, anti-sycophancy, and canned-response regression.

## Product Capabilities

| Area | Current capability |
| --- | --- |
| Desktop app | Native C++ WebView2 shell with tray controls and bundled web UI |
| Backend | C++ localhost HTTP server on `127.0.0.1:8000` |
| Model path | Local Gemma via OpenAI-compatible `llama.cpp` server |
| Memory | SQLite messages, sessions, profile facts, summaries, emotions, techniques |
| Retrieval | Keyword/relevance ranking over local memory and source material |
| Persona | Mode-scoped prompt kernel for social, task, reflective, counseling, direct, and crisis turns |
| Tools | Deterministic math/date/file/url/weather/PDF paths before model fallback |
| UI | Native-hosted chat UI with sessions, status panel, privacy reset, attachment text ingestion |
| Testing | PowerShell smoke suite against the native `/api/chat` and health endpoints |
| Privacy | Local databases, logs, model files, and personal paths are ignored by git |

## Architecture

```text
symbion_native.exe
  WebView2 desktop shell
  tray/menu/provider controls
  starts and monitors backend

symbion_backend.exe
  C++ HTTP server
  chat/runtime APIs
  SQLite memory store
  native deterministic tools
  local Gemma client

native/web/
  bundled HTML/CSS/JS chat client
  served by the backend

docs/
  memory architecture
  persona/prompt notes
  setup/testing/roadmap
```

Important source areas:

- [`native/src/app.cpp`](native/src/app.cpp): chat pipeline, routing cascade,
  tool dispatch, memory retrieval, model fallback, response repair, telemetry.
- [`native/src/gemma_client.cpp`](native/src/gemma_client.cpp): local model
  request path, system prompt assembly, session summarization.
- [`native/src/memory_store.cpp`](native/src/memory_store.cpp): SQLite schema,
  message/session/profile/emotion/summary/technique storage and retrieval.
- [`native/src/symbion_shell.cpp`](native/src/symbion_shell.cpp): WebView2 host,
  tray, backend lifecycle, provider switching, Gemma controls.
- [`scripts/smoke-conversation.ps1`](scripts/smoke-conversation.ps1): behavioral
  regression suite.

## Current Runtime Surface

The backend exposes:

- `GET /health`
- `POST /api/chat`
- `GET /api/local-gemma/status`
- `GET /api/messages/recent`
- `GET|POST /api/emotions`
- `GET /api/emotions/recent`
- `GET|DELETE /api/sessions`
- `GET /api/sessions/{id}/messages`
- `GET /api/profile/fact`
- `GET /api/memory/relevant`
- `GET|POST|DELETE /api/techniques`
- `GET|POST /api/techniques/sync`
- `POST /api/forget`

## Recent Build Milestones

- Migrated the tracked app from Electron/Python to native C++/WebView2.
- Added native SQLite-backed message, session, emotion, profile, summary, and
  technique storage.
- Added Local Gemma chat through an OpenAI-compatible local server.
- Added prompt mode separation so concrete task answers stay task-focused while
  reflective/counseling turns stay emotionally precise.
- Added signal-in-C++ / voice-in-model turn hints for social, everyday, and
  emotional turns.
- Added Gemma-driven session summarization through a storage-agnostic
  `SummaryGenerator` boundary.
- Added response retry/repair paths for generic local model misses.
- Added native deterministic tools and tool-result honesty.
- Added WebView2 shell self-healing for backend restarts and protected the
  backend from stalled browser sockets with threaded request handling.

## Build

Requirements:

- Windows 10/11
- CMake
- Visual Studio 2022 Build Tools with the C++ workload
- Microsoft WebView2 Runtime
- Microsoft WebView2 SDK package extracted locally or supplied with
  `-WebView2SdkRoot`

Build:

```powershell
.\scripts\build-native.ps1 -WebView2SdkRoot "C:\path\to\Microsoft.Web.WebView2"
```

## Run

```powershell
.\scripts\start-native.ps1
```

The shell starts the backend and opens the bundled WebView UI. By default, the
backend listens on `127.0.0.1:8000`.

Useful checks:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health
Invoke-RestMethod http://127.0.0.1:8000/api/local-gemma/status
Invoke-RestMethod http://127.0.0.1:8000/api/chat -Method Post -ContentType 'application/json' -Body '{"message":"I feel overwhelmed today"}'
```

## Test

```powershell
cmake --build native\build --config Release
.\native\build\Release\symbion_backend.exe --repo .
.\scripts\smoke-conversation.ps1
```

The smoke suite checks behavior rather than exact canned strings where possible:
memory scoping, task-mode restraint, direct answers, emotional mapping, source
honesty, local tool paths, anti-support-bot phrasing, crisis handling, and
regression around local model generic misses.

## Package

```powershell
.\scripts\package-native.ps1
```

The package script stages native binaries, config, web UI, and source text into
`native/dist/`.

## Client-Facing Notes

This project is useful as a reference for:

- local-first AI desktop apps,
- C++/WebView2 product shells,
- lightweight local HTTP backends,
- local LLM integration,
- SQLite memory and retrieval,
- deterministic tool routing,
- behavioral regression testing for AI products.

See [`docs/CLIENT_OVERVIEW.md`](docs/CLIENT_OVERVIEW.md) for a shorter
business-facing summary.

## Documentation

- [Client overview](docs/CLIENT_OVERVIEW.md)
- [Setup](docs/SETUP.md)
- [Testing](docs/TESTING.md)
- [Memory architecture](docs/MEMORY_ARCHITECTURE.md)
- [Local Gemma runtime](docs/LOCAL_GEMMA.md)
- [Persona kernel](docs/PERSONA_KERNEL.md)
- [Prompt modules](docs/PROMPT_MODULES.md)
- [Native migration notes](docs/WEBVIEW2_MIGRATION.md)
- [Roadmap](docs/ROADMAP.md)

## Public Repo Notes

- `config/symbion.json` is a public-safe default config.
- Local databases, logs, model files, generated packages, private memories, and
  personal runtime paths are not tracked.
- No release artifacts are published yet; build from source for now.
- License has not been selected yet. Until a license is added, all rights are
  reserved by default under GitHub's public repository behavior.
