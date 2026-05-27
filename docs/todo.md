# Symbion Todo

Current-version target: local-first, memory-rich, fast, emotionally steady Symbion.

This file tracks the current ship scope only. Future/backlog work lives in
[ROADMAP.md](ROADMAP.md).

Current open item groups: 0.

## Current Scope Summary

- [x] Local Gemma is the default LLM provider.
- [x] SQLite memory remains the durable store.
- [x] Memory is retrieved on demand instead of preloading everything.
- [x] Emotional check-ins are stored in SQLite.
- [x] Emotional conversations use a brief mirror/label plus one simple follow-up question.
- [x] The always-on prompt is measured and under the 200-line target.
- [x] The current web UI look and feel is preserved.
- [x] A thin native WebView2 scaffold exists under `native/`.
- [x] Full Python test suite is passing.

## Local Gemma Default

- [x] Add provider `local_gemma`.
- [x] Use base URL `http://127.0.0.1:8088/v1`.
- [x] Use model id `local-gemma`.
- [x] Reuse OpenAI-compatible `/chat/completions`.
- [x] Support streaming responses.
- [x] Support non-streaming JSON-ish classifier calls without `response_format`.
- [x] Health-check `/v1/models`.
- [x] Read CodeCat config from `c:\projects\codecat\runtime\config\codecat.server.json`.
- [x] Add optional autostart using `c:\projects\codecat\runtime\scripts\start-gemma.ps1`.
- [x] Expose local Gemma runtime status through `/api/local-gemma/status`.
- [x] Add local Gemma to CLI provider choices.
- [x] Add local Gemma to runtime `/provider`.
- [x] Add local Gemma to the desktop provider switcher.
- [x] Keep Anthropic/Groq/Kimi/OpenAI/DeepSeek/HF/Ollama as optional fallback or escalation providers.
- [x] Cap normal local Gemma responses with `local_gemma_max_tokens`.
- [x] Cap local Gemma prompt context with `local_gemma_context_char_budget`.
- [x] Cap raw recent turns with `local_gemma_recent_turns`.

## Memory System

- [x] Review current SQLite schema and memory flow.
- [x] Keep raw messages, summaries, profile, interactions, tasks, gaps, contradictions, techniques, and embeddings.
- [x] Add first-class `emotional_checkins`.
- [x] Add indexes for emotional check-ins.
- [x] Add `search_memory(query, scope, k)`.
- [x] Add `get_memory_item(source, id)`.
- [x] Add `read_session(session_id, limit)`.
- [x] Add `record_emotional_checkin`.
- [x] Add `search_emotional_history`.
- [x] Fail closed when active-user context is missing.
- [x] Scope memory reads by active user.
- [x] Add prompt memory budget for local Gemma.
- [x] Keep deep/older recall on demand through tools.

## Emotional Conversation Mode

- [x] Add emotional-processing detector.
- [x] Detect venting, distress, confusion, counsel-like asks, and intense statements.
- [x] Define response shape: one brief mirror/label, one simple follow-up question, stop.
- [x] Prevent bullet-list slop unless the user asks for a plan/checklist/summary.
- [x] Avoid lawyerly disclaimers unless direct safety risk appears.
- [x] Keep structural/code tasks from being forced into emotional mode.
- [x] Add prompt-level tests for one-question/no-list behavior.

## Emotional Check-Ins

- [x] Store `id`, `timestamp`, `session`, `user`, `emotion`, `intensity`, `valence`, `body_location`, `trigger`, `note`, `source_message_id`, `confidence`, and `captured_by`.
- [x] Persist detector-created check-ins.
- [x] Support manual terminal check-ins with `/checkin`.
- [x] Support terminal history with `/emotions`.
- [x] Add web API: `GET /api/emotions`.
- [x] Add web API: `POST /api/emotions`.
- [x] Add sidebar `Emotions` tab.
- [x] Add quick check-in UI.
- [x] Filter by user and emotion.
- [x] Document local-only privacy posture.
- [x] Verify the UI with Playwright.

## Prompt And Persona

- [x] Distill `MasterDocument.docx` into `docs/COUNSELING_CANON.md`.
- [x] Keep the counseling canon as source guidance, not prompt bulk.
- [x] Add prompt-module docs in `docs/PROMPT_MODULES.md`.
- [x] Add prompt line-count helper.
- [x] Add tests for the always-on prompt line budget.
- [x] Keep always-on static prompt under 200 lines.
- [x] Guardrail high-voltage master-document material.
- [x] Do not default to demon/narcissist labeling.
- [x] Do not amplify paranoia or spiritual grandiosity.
- [x] Do not promise permanent escape from depression/anxiety.
- [x] Do not push trauma reliving without pacing and grounding.
- [x] Add longer-response escape hatch for explicit writing/code/review tasks.
- [x] Keep emotional mode to one simple follow-up question without blocking explicit work.
- [x] Add gentle optional intensity follow-up plus rating/number skip path.

## Web And Native Shell

- [x] Preserve current web UI look and feel.
- [x] Add Emotions sidebar without a marketing-style redesign.
- [x] Add native folder.
- [x] Add CMake scaffold.
- [x] Add Win32/WebView2 thin-shell scaffold.
- [x] Default shell URL to `http://127.0.0.1:8000/`.
- [x] Allow URL override with `SYMBION_WEBVIEW2_URL` or `--url`.
- [x] Document WebView2 migration plan.

## Docs

- [x] Update README for local-first direction.
- [x] Update setup docs for CodeCat Gemma.
- [x] Update commands docs for `local_gemma`, `/checkin`, and `/emotions`.
- [x] Update architecture overview.
- [x] Add local Gemma docs.
- [x] Add memory architecture docs.
- [x] Add prompt module docs.
- [x] Add counseling canon.
- [x] Add WebView2 migration docs.
- [x] Add next-version testing docs.

## Tests

- [x] Install portable Python for the repo.
- [x] Install pytest, pytest-asyncio, Playwright, and Chromium for verification.
- [x] Add local Gemma request-construction tests.
- [x] Add CodeCat config parsing tests.
- [x] Add local prompt-budget tests.
- [x] Add emotional check-in storage tests.
- [x] Add emotional check-in API tests.
- [x] Add emotional mode prompt tests.
- [x] Add memory tool scoping tests.
- [x] Add prompt line-budget tests.
- [x] Run `py_compile` on edited Python.
- [x] Run full Python suite: `143 passed, 12 skipped`.
- [x] Run `node --check electron/main.js`.
- [x] Verify `symbion.json` parses.
- [x] Run Playwright UI smoke for the Emotions tab.

## Current Open Items

- [x] Track emotional analytics signals in SQLite.
  - [x] Emotion over time.
  - [x] Stress over time.
  - [x] Peace/hope over time.
  - [x] Trigger/event markers.
  - [x] Practices that helped.
  - [x] Positive change markers.
  - [x] Negative change markers.
  - [x] Keep the data shape ready for future graphs and exports.
- [x] Add memory correction UX beyond existing forget behavior.
  - [x] "That memory is wrong."
  - [x] "Remember this."
  - [x] "Do not bring this up unless I ask."
  - [x] User-visible edit/delete for emotional check-ins.
- [x] Improve summaries.
  - [x] Episode summary format.
  - [x] Include people, projects, decisions, emotional context, and open loops.
  - [x] Include freshness and confidence.
  - [x] Include "do not mention unless relevant" flags for sensitive items.
- [x] Improve consolidation.
  - [x] Preserve source sessions.
  - [x] Avoid losing emotionally important detail.
- [x] Import `MasterDocument.docx` into runtime memory.
  - [x] Extract text.
  - [x] Chunk by section.
  - [x] Store chunks in SQLite.
  - [x] Tag chunks.
  - [x] Retrieve only when relevant.
  - [x] Prefer gentle/practical chunks.
  - [x] Keep high-intensity chunks out of default retrieval.
  - [x] Never let source chunks override crisis safety.
  - [x] Define candidate tags:
    - [x] `mindfulness`
    - [x] `journaling`
    - [x] `grief`
    - [x] `confession`
    - [x] `safe_listener`
    - [x] `marriage_repair`
    - [x] `forgiveness`
    - [x] `jesus_now`
    - [x] `spiritual_warfare`
    - [x] `high_intensity_do_not_default`
- [x] Bring WebView2 shell to Electron parity.
  - [x] Tray show/hide/quit behavior.
  - [x] Single-instance lock.
  - [x] Backend process start/stop.
  - [x] Gemma process start/stop/status ownership.
  - [x] Provider switcher.
  - [x] Update checker or replacement update flow.
  - [x] Local auth/key handling.
  - [x] Analytics/status window.
- [x] Verify native build and packaging.
  - [x] Install CMake and MSVC/Build Tools or equivalent.
  - [x] Verify WebView2 SDK discovery.
  - [x] Compile native scaffold.
  - [x] Package native app.
  - [x] Decide when to switch default desktop launcher.
  - [x] Deprecate Electron only after feature parity.
- [x] Add `list_related_sessions(query)`.
- [x] Add `get_profile_fact(key)`.
- [x] Finish source-label formatting for retrieved memory tool results.
- [x] Add a short release note / changelog entry.
- [x] Update threat-model notes for machine-wide file writes.
