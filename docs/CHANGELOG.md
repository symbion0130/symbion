# Changelog

## 2026-05-27 Native C++ Runtime Pivot

- Removed Electron source, package metadata, assets, and installer script.
- Removed the tracked Python runtime, launchers, package metadata, tests, eval harness, and helper scripts.
- Added `symbion_backend.exe`, a lightweight C++ localhost backend scaffold.
- Updated the WebView2 shell to launch the C++ backend directly.
- Kept SQLite data files local and untouched.
- Added native SQLite message storage and emotional signal tracking.
- Added native Local Gemma chat calls and `/api/chat`.
- Replaced the placeholder web page with a native chat UI.
- Moved root config and source documents into `config/` and `docs/source/`.
- Tuned native counseling prompts around rooted calm, stress reduction, and delicate memory reopening.
- Adjusted memory retrieval to prefer user-authored memories and avoid treating the current turn as old memory.
- Added native forget/delete-memory flow for natural language memory removal.
- Added wipe-all memory reset flow and UI control.
- Added native Tier 1/Tier 2 tool handling for exact date/time/math, local file and directory reads, URL fetch, lightweight web search, weather lookup, and text-based PDF extraction.
- Added native session history loading through `/api/sessions/{id}/messages` and a compact recent-session rail in the WebView UI while keeping new chat as the launch default.
- Switched shared technique learning hashes to Windows SHA-256 so native shared-learnings sync matches the v14 hash contract.
- Added v14-style native JSONL turn telemetry under `data/symbion_events.jsonl`, including intent, response source, latency, memory counts, emotion, and stale-refresh flags.
- Added one-shot stale-answer refresh: if Local Gemma drafts knowledge-cutoff/no-browse language on a current-looking query, the native backend runs a lightweight web search and retries once with that result.
- Added `scripts/eval-native-v14.ps1` to run v14 JSONL golden cases against the native `/api/chat` path without restoring Python.
- Prevented low-signal social greetings and correction turns from pulling unrelated cross-session memories into the prompt.
- Added JSON `\uXXXX` decoding so apostrophes and other escaped characters do not become literal `u0027` text in messages or session titles.
- Removed stale merge-conflict markers from the native chat path and testing docs.
- Threaded active user through native message saves, emotion saves, session listing, profile facts, and relevant-memory retrieval.
- Made the native shell ignore accidental hidden startup flags so the main window opens visibly.
- Embedded the Symbion `.ico` in the native shell and use it for the window, taskbar, and tray icon.
