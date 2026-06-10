# Symbion Client Overview

Symbion is a native Windows AI assistant built around local execution,
long-running memory, deterministic tools, and a model-backed conversational
interface. The current implementation is a C++/WebView2 desktop app with a C++
localhost backend and SQLite storage.

## What Has Been Built

### Native Desktop Runtime

- WebView2 shell written in C++.
- Native tray/menu lifecycle controls.
- Backend start/stop/restart ownership from the shell.
- Provider switching hooks.
- Local Gemma controls.
- WebView UI hosted from the local backend.
- Recovery work for hidden startup, stalled backend, and WebView black-screen
  failure modes.

### Local C++ Backend

- Lightweight HTTP server on `127.0.0.1`.
- Threaded request handling so stalled browser sockets do not block health or
  chat traffic.
- Health, chat, session, emotion, memory, technique, local-model, and
  forget/reset endpoints.
- Static web UI and asset serving from the same native process.

### Local Model Integration

- Local Gemma chat path through an OpenAI-compatible `llama.cpp` server.
- Mode-aware system prompts.
- Social/everyday/emotional turn hints.
- Retry guidance for generic local model misses.
- Response-source telemetry for debugging and evaluation.
- Gemma-driven session summarization through a storage-agnostic interface.

### Memory And Retrieval

- SQLite message and session storage.
- User-scoped memory.
- Profile facts.
- Emotion signals and check-ins.
- Session summaries.
- Relevant-memory retrieval.
- Shared technique storage and sync.
- Forget and wipe flows.

### Native Tools

Symbion routes deterministic requests before model fallback:

- exact math,
- date/time,
- local file reads,
- directory listing,
- URL fetch,
- lightweight web search,
- weather lookup,
- text-based PDF extraction.

### Behavioral Reliability

The smoke suite exercises product behavior rather than only endpoint existence:

- direct answer correctness,
- task-mode restraint,
- social warmth without support-bot phrasing,
- emotional one-door-at-a-time mapping,
- crisis route behavior,
- memory scoping,
- session recall,
- source honesty,
- tool-result honesty,
- local model fallback and repair paths.

## Engineering Themes

This project demonstrates:

- building a native AI desktop product without Electron,
- integrating local LLMs behind an OpenAI-compatible interface,
- designing local memory systems with SQLite,
- separating deterministic system behavior from model-generated voice,
- maintaining behavior with smoke tests,
- keeping private data local while making the product architecture visible.

## What A Client Can Evaluate From This Repo

- Architecture: a complete native shell/backend/model/memory stack.
- Product judgment: clear separation between task, social, reflective,
  counseling, direct-answer, and crisis paths.
- Reliability work: self-healing backend startup, threaded HTTP handling, and
  WebView diagnostics.
- AI engineering: prompt mode separation, turn hints, summarization boundaries,
  fallback paths, and regression tests for behavioral quality.
- Practical delivery: scripts for build, run, package, and smoke testing.

## Current Gaps

- No packaged public release artifact yet.
- No selected open-source license yet.
- Semantic embedding retrieval is intentionally not presented as complete.
- Some advanced analytics and dashboards remain roadmap items.

## Suggested Client Demo Path

1. Build the native binaries.
2. Start the app with `.\scripts\start-native.ps1`.
3. Check `GET /health`.
4. Run `.\scripts\smoke-conversation.ps1`.
5. Open the WebView shell and inspect:
   - chat,
   - session list,
   - runtime status,
   - privacy reset,
   - local model status.

For technical details, start with:

- [`README.md`](../README.md)
- [`MEMORY_ARCHITECTURE.md`](MEMORY_ARCHITECTURE.md)
- [`TESTING.md`](TESTING.md)
- [`LOCAL_GEMMA.md`](LOCAL_GEMMA.md)
