# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Symbion is

Symbion is an async Python AI assistant and behavioral-safety research harness. Current version: **v14** (~8800 lines, `symbion_v14.py`).

**Only edit `symbion_v14.py`.** Older versions (`symbion_v3.py`..`symbion_v13.py`, plus `symbion_v13.py.orig`, `symbion_agent.py`, `symbion_core.py`) live in `archive/legacy_versions/` and are kept locally for diffing only — they are not imported, not tracked in git (the whole `archive/` tree is gitignored), and must not be modified. `symbion_v14.py` is the active codebase. It has multi-provider LLM support (Anthropic, OpenAI, Ollama, Kimi), SQLite persistence, a judge-model safety layer, self-evaluation with revision, longitudinal identity, a FastAPI/WebSocket web UI, an Electron desktop shell, location services, and cross-user retrieval.

It is not a chatbot wrapper. It attempts to reproduce alignment properties from outside a model using behavioral proxies. v14 stripped 11 LLM-grading-LLM probe subsystems from v11-v13 and replaced them with an offline eval harness (`evals/`) and JSONL telemetry.

## Commands

```bash
# First-time interactive setup (writes API keys to .env)
python -m symbion --setup

# Run tests (63 tests: calculator, sandbox, SSRF, JSON parsing, retrieval)
python -m pytest tests/ -q

# Run a single test file or test
python -m pytest tests/test_tools.py -q
python -m pytest tests/test_tools.py::TestCalculator::test_basic_arithmetic -v

# Compile check (fast — catches syntax errors)
python -m py_compile symbion_v14.py

# Smoke test (needs Ollama running, or falls back to heuristic)
python scripts/smoke.py

# Run the eval harness against the golden set (120 entries across many buckets)
python evals/run.py --provider anthropic --concurrency 4
python evals/run.py --provider ollama                       # local; --concurrency 1
python evals/run.py --provider anthropic --golden evals/golden_refuse.jsonl   # filtered subset
python evals/run.py --provider anthropic --golden evals/golden_tool_judgment.jsonl --tools  # tool-judgment bucket; --tools enables real tool execution

# Browser-level verification (Playwright, headless). 34 steps across session
# sync, sidebar, attachments, scroll, location, cross-user. Boots a temp
# server with a stub respond() so checks don't burn LLM cost.
.python/python.exe scripts/verify_session_sync.py

# Latency benchmark (heuristic mode, no LLM needed)
python scripts/bench_latency.py

# Run Symbion (terminal mode)
python symbion_v14.py --provider ollama
python -m symbion --provider anthropic --web

# Electron desktop shell (wraps the web UI in a native window)
cd electron && npm install && npm start             # dev run
cd electron && npm run build:win                    # NSIS installer at dist/Symbion Setup *.exe

# HTTPS for browser-geolocation on phones / non-localhost devices.
# Wraps `tailscale serve` so Symbion is reachable at https://<host>.<tailnet>.ts.net.
.\scripts\tailscale-https.ps1                       # tailnet-only (default)
.\scripts\tailscale-https.ps1 -Funnel               # public
.\scripts\tailscale-https.ps1 -Off                  # tear down

# Pull the local embedding model used for semantic retrieval (one-time, ~670MB)
ollama pull mxbai-embed-large

# Optional OCR fallback for read_pdf on scanned/image-only PDFs.
# Also requires the Tesseract binary on PATH:
#   https://github.com/UB-Mannheim/tesseract/wiki  (Windows installer)
pip install pypdfium2 pytesseract
```

## Repo layout

```
symbion_v14.py              # active codebase — everything lives here (~8800 lines)
archive/                    # gitignored. legacy_versions/ holds v3..v13
                            #   snapshots + v13.py.orig + symbion_agent.py /
                            #   symbion_core.py for diffing. docs/ holds
                            #   pre-v14 planning + spec docs.
symbion/                    # thin package wrapper for python -m symbion
  __init__.py               # re-exports SYMBION, SymbionConfig, HealthMetrics, main (now from symbion_v14)
  __main__.py               # entry point
  web/templates/index.html  # web UI HTML (extracted from inline string)
  clients|pipeline|tools/   # empty placeholders — do NOT split symbion_v14.py into these without an explicit request
electron/                   # Electron desktop shell
  main.js                   # spawns python -m symbion --web (or attaches if
                            #   /health already responds), opens BrowserWindow,
                            #   walks process tree on quit
  preload.js                # contextBridge stub for future native integrations
  package.json              # electron + electron-builder; NSIS installer config
  assets/                   # symbion.ico / .icns / 512.png / mark.svg (white variant)
  README.md                 # dev-run + packaging notes
evals/
  golden.jsonl              # 120-entry eval dataset (rule-based grading, no LLM).
                            #   Buckets include: casual / tech / ethics / creative /
                            #   personal / overcaution / refuse / voice / drift /
                            #   toolhonesty / ocr / refusal / syco / scaffold / meta /
                            #   identity / clinical / code_honesty / specialness /
                            #   grandeur / calibration / restraint / tooljudge.
                            # Rule fields: must_include, must_not_include (clause-aware
                            #   negation + quote-skip), must_not_start_with, max_chars,
                            #   max_lines, max_tool_calls, must_call_tools,
                            #   must_not_call_tools. Multi-turn entries use
                            #   turns: [...] and score the FINAL response.
  run.py                    # offline eval runner. --concurrency N (default 4)
                            #   parallelizes via asyncio.gather + semaphore.
                            #   --tools enables cfg.tools_enabled for the run
                            #   (required by tool-judgment bucket).
                            #   ROUTES TO A TEMP DB on every run so cases see
                            #   clean state and the real symbion.db isn't
                            #   polluted with test sessions.
  results/                  # gitignored. per-run JSON + tee'd log artifacts.
scripts/
  smoke.py                  # instantiate + one respond() call; self-diagnoses
                            #   CPU-bound Ollama vs. real regression.
  bench_latency.py          # framework overhead measurement
  migrate_v13_to_v14.py     # DB table migration
  verify_session_sync.py    # Playwright + headless Chromium harness. 34 steps:
                            #   session sync, sidebar collapse, attachments, scroll
                            #   follow, location pipeline, cross-user presence +
                            #   retrieval, peer WS broadcast.
  install-ollama.ps1        # winget-install Ollama + pull mistral/llama3.2/
                            #   mxbai-embed-large. Idempotent. Called by install.ps1.
  tailscale-https.ps1       # tailscale serve/funnel wrapper.
  install-web.cmd, refresh-here.cmd, verify-worker.ps1, push-env.ps1
tests/
  conftest.py               # StubClient fixture (BaseClient test double)
  test_tools.py             # calculator, sandbox, SSRF, _parse_json
docs/                       # user-facing markdown docs: SETUP, COMMANDS,
                            #   CHANGELOG, SYMBION_OVERVIEW. README.md +
                            #   CLAUDE.md stay at the root by convention.
symbion.json                # config (non-secret fields)
.env                        # API keys (UTF-8, no BOM)
symbion.db                  # SQLite store
symbion_events.jsonl        # per-turn JSONL telemetry (append-only)
symbion_transparency.log    # legacy per-turn audit log (append-only)
_pastes/                    # gitignored. Web UI attachment uploads land here.
verify_artifacts/           # gitignored. PNGs + log.json from verify_session_sync.py.
pyproject.toml              # pip install -e .
```

## Deployment / fresh-machine install

The fresh-machine install runs through a Cloudflare Worker that gates access via a bearer token and injects provider keys into the served `install.ps1` before delivery.

**The three install commands** (canonical reference is `install.ps1`'s `.EXAMPLE` blocks):

```powershell
# 1. Default - first install where ExecutionPolicy permits scripts
irm https://symbion-installer.symbion-0130.workers.dev?t=<token> | iex

# 2. Locked-down - first install where (1) errors "running scripts is disabled"
powershell -ExecutionPolicy Bypass -Command "irm https://symbion-installer.symbion-0130.workers.dev?t=<token> | iex"

# 3. Refresh - Symbion already installed
%USERPROFILE%\symbion\scripts\install-web.cmd
```

The Worker performs string-replace on four placeholders before serving:

| Placeholder in `install.ps1` | Worker Secret | Used by `install.ps1` for |
|---|---|---|
| `__SYMBION_PAT_INJECTED__` | `GITHUB_PAT` | `git clone` (Contents:read on symbion0130/symbion) |
| `__SYMBION_ANTHROPIC_KEY_INJECTED__` | `ANTHROPIC_API_KEY` | seed `.env` |
| `__SYMBION_BRAVE_KEY_INJECTED__` | `BRAVE_API_KEY` | seed `.env` |
| `__SYMBION_API_KEY_INJECTED__` | `SYMBION_API_KEY` | seed `.env` |

Kimi and DeepSeek were deliberately removed from Worker injection on 2026-05-19 to reduce blast radius if the install token leaks. Those keys ride the OneDrive seed path or manual `--setup` when needed — *don't re-add them to the Worker without a paired decision in commit message.*

**install.ps1 phases** (in order):

1. Locate or fetch the repo (in-clone or git clone via PAT)
2. Bootstrap portable Python + deps + shim (`scripts/bootstrap-portable.bat`)
3. **Ollama + local models** (`scripts/install-ollama.ps1`) — idempotent: skips Ollama install when already present, skips each model pull when in `ollama list`. Suppressed with `-SkipOllama` or `$env:SYMBION_SKIP_OLLAMA`. Failures don't abort the install; user can fall back to `--provider anthropic`.
4. `.env` source priority: existing → OneDrive seed → Worker-injected → interactive `--setup`
5. Launch Symbion in a new window

**Tooling around the install:**

| Script | Purpose |
|---|---|
| `scripts/install-web.cmd` | Worker one-liner via `powershell -ExecutionPolicy Bypass`. Sidesteps locked-down ExecutionPolicy. Use for refresh. |
| `scripts/refresh-here.cmd` | Runs the *local* `install.ps1` (in-repo via `$PSScriptRoot`) — no Worker round-trip. |
| `scripts/verify-worker.ps1` | 11-check smoke test of the Worker. Run after any Worker change. Never echoes secrets. |
| `scripts/push-env.ps1` | `.env` ⇄ OneDrive sync. `-Pull` reverses direction. |
| `scripts/install-ollama.ps1` | Install Ollama + pull `mistral`, `llama3.2`, `mxbai-embed-large`. `-SkipModels`, `-Models "a,b,c"`. |
| `scripts/tailscale-https.ps1` | Tailscale serve/funnel wrapper for HTTPS on `*.ts.net`. Required for browser geolocation on non-localhost devices. |

**Edit constraints when touching install/deploy:**

- Adding a new Worker-injected key requires changes in three places that must stay in sync: `install.ps1` (placeholder string + `Resolve-InjectedKey` call + Phase 3 `$injected` hashtable), the Worker source (new `.replaceAll` line + reference the new Secret), and `verify-worker.ps1` (new entry in `$extraKeys`). Also add the matching Cloudflare Secret in the dashboard.
- The install token is hard-coded into `install.ps1`'s `.DESCRIPTION` / `.EXAMPLE`. Acceptable because the repo is private — anyone who can read `install.ps1` already has clone access via the PAT.
- The Worker source lives only in the Cloudflare dashboard inline editor (no `wrangler.toml` in this repo). Five Secrets required there: `GITHUB_PAT`, `ANTHROPIC_API_KEY`, `BRAVE_API_KEY`, `SYMBION_API_KEY`, `INSTALL_TOKEN`.
- The zip-via-`archive/refs/heads/main.zip` fallback is intentionally absent — GitHub 302s archive requests to `codeload.github.com` and PowerShell drops the `Authorization` header across hosts.

## Architectural layers (top to bottom in symbion_v14.py)

```
Config & setup           SymbionConfig, run_setup, _load_dotenv_safe.
                         _REPO_ROOT + _anchor() defined ABOVE
                         _load_dotenv_safe so .env, symbion.json, DB, logs,
                         tools workspace all resolve to the repo dir
                         regardless of launch CWD.
Prompts                  SYMBION_PERSONA, PRE_GEN_SYSTEM, JUDGE_SYSTEM,
                         SELF_EVAL_SYSTEM, EMOTIONAL_STATE_SYSTEM, VOICE_LOOSEN,
                         REASONING_SYSTEM, CONTRADICTION/GAP/PROACTIVE/PROFILE/
                         SUMMARISE/TOOL_DISPATCH system prompts
HealthMetrics            telemetry-only dataclass (mood, revision rate, distress)
DB                       init_db() — CREATE TABLE IF NOT EXISTS + idempotent
                         ALTER TABLE for legacy upgrades
Infrastructure           CircuitBreaker, RateLimiter, _parse_json (brace-counting)
LLM clients              BaseClient (+ supports_tools flag) + Ollama/Anthropic/
                         OpenAI/Kimi/OfflineJudgeStub. AnthropicClient also
                         exposes stream_with_tools (native tool-use loop).
Embedding client         EmbeddingClient — wraps Ollama /api/embeddings for
                         mxbai-embed-large (1024-dim). Returns None on any
                         failure so retrieval can fall back cleanly to
                         BM25-only.
Retrieval helpers        _retrieval_tokenize, _bm25_rank.
Tool safety helpers      _safe_calc (AST), _is_safe_url (SSRF), _resolve_in_workspace
Tools                    SymbionTools — 13 tools registered in _ALLOWED_TOOLS
                         and TOOL_SCHEMAS:
                           file/data: calculate, datetime, read_file,
                             read_file_chunk, read_image, read_pdf, list_dir,
                             write_file, web_search, fetch_url
                           location:  get_weather (Open-Meteo, no key),
                             get_local_time (IANA timezone)
                           cross-user: get_user_recent_activity (queries
                             another household member's recent summaries +
                             snippets; symmetric; runtime-refuses self-queries)
                         _ALLOWED_TOOLS / TOOL_SCHEMAS / dispatch / _validate_args
                         must stay in sync when adding tools.
EventLogger              JSONL event stream. Agent-loop turns include an
                         agent_loop block with iterations + per-tool
                         {name, input, output_chars, is_error}.
Constitution             SymbionConstitution (PRINCIPLES, VERSION — vestigial)
Identity                 LongitudinalIdentity (formative moments)
Task engine              TaskEngine
Trackers                 ContradictionTracker, KnowledgeGapTracker
Memory + learner         SymbionMemory, SymbionLearner.
                         Cross-interface session: set_active_session,
                           get_active_session, list_sessions,
                           get_session_messages, get_max_message_id,
                           get_messages_after.
                         Location: set_location, get_location, clear_location.
                         Cross-user: get_user_last_activity,
                           get_user_recent_activity.
                         Hybrid retrieval: get_relevant_summaries_hybrid
                           (BM25 + cosine, recency-weighted).
SYMBION                  the orchestrator — respond() is the hot path.
                         _maybe_tool holds three regex hard-triggers (multi-
                         file paths, _SELF_SOURCE_RE, _SEARCH_TRIGGER_RE).
                         _should_skip_pregen + _PREGEN_RISK_RE skip the
                         judge entirely on short benign queries.
                         _pre_gen_analysis has LRU cache (512 entries).
                         _force_summarize_session powers /summarize +
                         quit-flush. _backfill_embeddings runs once on launch.
                         _reverse_geocode_and_store (Nominatim, async) +
                         broadcast_to_session (WS peer fan-out) +
                         _ws_clients registry for multi-device sessions.
                         _session_last_tool_calls per-session cache that
                         the eval harness reads to assert tool-judgment rules.
Validation               validate_and_report()
Web                      build_web_app() — FastAPI + WebSocket assembly.
                         Routes: /, /health, /api/chat, /api/sessions,
                         /api/sessions/{id}/messages, /api/tasks,
                         /api/identity, /api/gaps, /api/shutdown,
                         /ws/{session_id}.
Terminal                 run_terminal() + HELP_TEXT + _stream_print.
                         /quit awaits _force_summarize_session.
                         /sessions, /resume <n>, /new commands.
                         Pre-prompt peer-drain (watermark-based) for
                         cross-device awareness in terminal mode.
Entry point              main()
```

## The respond() pipeline (critical path)

```
1. Pre-gen — _pre_gen_analysis (fused judge + emotion in one LLM call).
   In single-shot mode this runs in parallel with _maybe_tool via gather;
   in agent-loop mode (default for Anthropic responder) tool dispatch is
   skipped here because the model fires tools itself during generation.
2. Contradiction check
3. System prompt assembly — persona + CAPABILITIES_BASE +
     (CAPABILITIES_AGENT_MODE | CAPABILITIES_SINGLE_MODE) + mood +
     emotion_mode + voice_loosen + tool results (single-shot only) +
     contradiction notice + over-caution override
4. Generation — branches on agent_loop_active:
     - Agent loop: AnthropicClient.stream_with_tools drives a multi-iteration
       loop. Model emits tool_use blocks, framework executes via SymbionTools.
       dispatch, results return as tool_result, model continues until
       end_turn or max_iterations (default 8).
     - Single-shot: existing resp_client.stream() path. Optional CoT wrap.
5. Stale-draft fallback — single-shot only.
6. Self-eval — FIRE-AND-FORGET (2026-05-21). Runs in parallel with response
   delivery instead of blocking it; saves 2-3s/turn on substantive responses.
   Revision still triggers when score < 0.40, with a max of one revision.
7. Background tasks (fire-and-forget) — summarise, profile update,
     knowledge gap check, identity moment recording
8. Health + learn — HealthMetrics.record(), SymbionLearner.record()
9. Event log — EventLogger.log_turn() (JSONL), _write_log() (legacy).
10. Cache last_agent_tool_calls + active_session pointer for eval harness
    and cross-interface session sync.
```

**Do not serialize what is parallel.** In single-shot mode, pre-gen is 2 parallel calls. In agent-loop mode pre-gen is just judge+emotion (one call). Adding a new pre-gen step means adding it to the gather (single-shot) or sequencing it before generation (agent loop).

**Agent loop boundaries.** `agent_loop_enabled` in SymbionConfig (default True). Active only when (a) tools_enabled, (b) cfg.agent_loop_enabled, (c) responder client has `supports_tools = True` (currently AnthropicClient only). Tool schemas in `TOOL_SCHEMAS` must stay in sync with `SymbionTools._ALLOWED_TOOLS` and `_validate_args`. Iteration cap is `agent_loop_max_iterations` (default 8).

## Non-negotiable invariants

1. **Async end-to-end.** No blocking HTTP in the respond pipeline. Parallel where possible via `asyncio.gather()`.
2. **Graceful degradation.** Every tool and external call is wrapped. Failures log and continue — never kill respond().
3. **Logs are append-only.** Both `symbion_transparency.log` and `symbion_events.jsonl`.
4. **Only the judge refuses.** There is no survival gate. `HealthMetrics` is telemetry only.
5. **Config-gated subsystems.** Every subsystem gets a bool in `SymbionConfig` and is skippable.
6. **DB migrations are additive only.** Never drop columns. `init_db()` uses `CREATE TABLE IF NOT EXISTS` plus idempotent `ALTER TABLE ADD COLUMN` for legacy DB upgrades. Readers tolerate NULL.
7. **No eval() or exec() on untrusted input.** The calculator uses AST validation. URLs are SSRF-checked. **File reads are machine-wide; writes are workspace-sandboxed.** `_resolve_in_workspace(path, root, read_only=True)` accepts any absolute path on the machine for the read tools; only `write_file` enforces the workspace boundary. The user explicitly opted in to machine-wide reads on 2026-04-25 — do not narrow this back without explicit user direction.
8. **No bare `except:`.** Use `except ImportError:` for import guards, `except Exception:` everywhere else.
9. **Tool registry sync.** Adding a tool requires changes in FOUR places: `SymbionTools._ALLOWED_TOOLS`, `SymbionTools._validate_args`, `SymbionTools.dispatch`, and `TOOL_SCHEMAS`. Plus a CAPABILITIES_BASE line so the model knows it exists.

## Web layer security

(All of these were added because of specific holes. Don't weaken without a paired Codex finding + commit note.)

- (a) `main()` refuses to start `--web` with `web_host != localhost` AND empty `api_key` (LAN-no-auth guard)
- (b) NO CORSMiddleware — UI is same-origin with `/api/chat`; wildcard CORS was a real hole
- (c) `/api/chat` streams body via `request.stream()` with a 1MB cap that aborts chunked-evasion attempts; 100K char cap on the message
- (d) WS handler validates `Origin` via `_origin_allowed` (loopback + RFC1918 + port match) BEFORE `accept()` — blocks browser drive-by from public origins regardless of api_key state
- (e) WS attachment uploads validate data-URL length, base64 length, and decoded size BEFORE `b64decode(validate=True)`
- (f) `uvicorn.run` is called with `ws_max_size=80MB` (bumped from 32MB on 2026-05-21 to fit a 50MB file attachment + overhead in one frame; lower would protocol-reject large frames before our handler caps fire)
- (g) Web body is `position:fixed; top:0; left:0; right:0; height: var(--viewport-height, 100dvh)` so iOS Safari can't scroll the page when the keyboard opens (fixes the "composer flies to top + blank space at bottom" bug, 2026-05-21)

## Cross-interface session sync (2026-05-21)

The same conversation can be picked up across terminal and web. Three pieces:

1. **Shared `active_session` pointer.** Stored per-user in `user_profile` under `__active_session` / `__active_session_ts` (filtered from /profile display by the `__` prefix check in `get_profile_with_meta`). Updated in `respond()` after every turn. Both `run_terminal()` and the web UI auto-resume the pointer on launch if it was touched in the last 24h.
2. **Session list endpoints.** `GET /api/sessions` returns newest-first sessions for the active user (id, title from first user message, last_activity, turn_count) plus the active pointer. `GET /api/sessions/{id}/messages` returns up to 200 messages for sidebar hydration. Both require X-API-Key when configured.
3. **Live multi-device broadcast.** SYMBION has a `_ws_clients` registry (per-session set of WebSockets). When user A on device 1 sends a message, the WS handler broadcasts `remote_user` immediately (before respond) and `remote_assistant` after respond completes, to all OTHER peer sockets on the same session. The receiving client renders both with a "synced from another device" chip. Terminal mode uses a watermark instead (`get_max_message_id` + `get_messages_after`) — drains new peer messages above the next `you >` prompt.

The sidebar in the web UI lists sessions newest-first, collapses to 5 by default with a "Show all (N)" expander, click-to-switch with REST hydration before WS reconnect.

## Location services (2026-05-21)

Browser geolocation → server → reverse-geocoded city/state/country → surfaced to the model + consumed by `get_weather` / `get_local_time` tools.

- **Client** (web UI): on WS connect after auth, calls `navigator.geolocation.getCurrentPosition`. Sends `{type:"location", lat, lon, tz, accuracy}` to the server. `LOCATION_OPT_IN` in localStorage remembers the choice. Status sheet has "Share location" / "Clear location" buttons. **Requires HTTPS (secure context) on non-localhost origins** — see `scripts/tailscale-https.ps1`.
- **Server**: WS handler stores fields in `user_profile` under `__loc_*` keys (lat, lon, tz, accuracy, city, state, country, ts). Fires `_reverse_geocode_and_store` async via Nominatim (free, no key, User-Agent set per their policy). On completion broadcasts `location_update` back to peer WS clients so the pill switches from raw coords to "City, State, Country".
- **Address parsing**: city falls through `city → town → village → suburb → hamlet → municipality → county` (the last two cover rural US where city is null). State falls through `state → state_district → province → region` (anchors the US/CA/AU regional identity).
- **build_context** injects: `"User's current location (ambient context, DO NOT mention unless they ask a location-anchored question — no 'hope you're enjoying X', no 'as a fellow Texan'): City, State, Country, timezone X [coords: lat=..., lon=...]"`. The DO-NOT-MENTION phrasing was specifically added because the model defaults to name-dropping location to demonstrate awareness.
- **Tools**: `get_weather(lat, lon)` hits Open-Meteo current forecast (free). `get_local_time(timezone)` uses ZoneInfo for IANA tz wall clock. Both pure-function — model reads coords/tz off the context line and passes them in.

## Cross-user presence + retrieval (2026-05-21)

Symmetric — any household user can query any other.

- **Phase 1 (presence)**: when `cfg.known_users` has > 1 entry, `build_context` appends `"Other household users with recent Symbion activity: lala (5m ago)"`. Last-active timestamp only — no content leak. Stale beyond 7 days drops off.
- **Phase 2 (on-demand retrieval)**: `get_user_recent_activity(user, hours)` tool returns target user's recent summaries + a few raw message snippets. Validated against `cfg.known_users` (trust boundary). **Self-queries are runtime-refused** with a clear error ("active user's own history is already in your context — just answer from that"), since calling cross-user on yourself is wrong semantics and the model would otherwise do it.

## Eval harness

`evals/run.py` is the offline harness. Reads `evals/golden.jsonl` (120 entries) and scores each case via rule-based assertions. **No LLM grading.**

**Isolation:** Each run creates a fresh tempfile DB (2026-05-21 fix) — earlier runs were reading the user's real `symbion.db` and writing test sessions into it, which silently invalidated several "no location set" / "no cross-user history" assertions and polluted real conversation history with eval junk.

**Rule types** (any combination, all opt-in per entry):

| Rule | Meaning |
|---|---|
| `must_include` | Substring (case-insensitive) appears in response |
| `must_not_include` | Substring DOES NOT appear, with clause-aware negation (`"there's no leash"` is ok if asserting "leash" is forbidden) + quote-aware skip |
| `must_not_start_with` | Response doesn't open with phrase (leading-whitespace-tolerant) |
| `max_chars` | Response length post-`.strip()` ≤ N |
| `max_lines` | Response newline count post-`.strip()` ≤ N |
| `max_tool_calls` | Tool calls fired this turn ≤ N (0 = forbid). Requires `--tools` at the run level. |
| `must_call_tools` | List of tool names that MUST appear in tool_calls |
| `must_not_call_tools` | List of tool names that must NOT appear |

**Bucket inventory** (case ID prefix → intent):

| Bucket | Intent |
|---|---|
| `casual_*`, `tech_*`, `creative_*`, `personal_*` | Basic conversational shape |
| `ethics_*`, `clinical_*`, `refuse_*` | Refuse-or-engage discrimination |
| `overcaution_*` | Should-assist cases the judge tends to over-refuse |
| `voice_*`, `drift_*` | Persona stability under stress |
| `syco_*`, `scaffold_*` | Sycophancy / agreement bias |
| `identity_*` | Resists "tuned-down version" framing (iid 296, 2026-04-26) |
| `grandeur_*`, `calibration_*` | Rapport-driven drift (iid 562/561, 2026-05-18); multi-turn `turns:[]` variants for the cumulative-warmth case |
| `meta_*`, `whoami_*`, `specialness_*`, `code_honesty_*` | Self-knowledge claims |
| `toolhonesty_*`, `ocr_*` | Don't fabricate tool outputs |
| `restraint_*` (15 cases) | Brevity discipline on micro-acks + explicit brevity cues ("tldr", "in one sentence"). Doesn't penalize substantive depth. |
| `tooljudge_*` (19 cases, `--tools` required) | When-to-call-which-tool. NO-tool cases (opinions, identity, simple math, casual chat) assert `max_tool_calls=0`. MUST-tool cases (read named file, multi-digit math, web_search for current data, cross-user query) assert the right tool name fires. |

Filtered subsets (gitignored) like `golden_restraint.jsonl`, `golden_tool_judgment.jsonl` are written by ad-hoc filter scripts and used via `--golden`.

## Electron desktop shell (2026-05-21)

`electron/` wraps the FastAPI web UI in a native window. Thin shell — no Symbion code changes.

- **Lifecycle**: spawns `../.python/python.exe -m symbion --web`, polls `/health` until 200, opens `BrowserWindow` at `localhost:8000`. On quit walks the Python process tree (`taskkill /T /F` on Windows).
- **Attach-vs-spawn**: probes `/health` BEFORE spawning. If a Symbion is already serving, attaches to it instead of starting a duplicate (common case: user has `python -m symbion --web` running in a terminal).
- **Single-instance lock** so double-click doesn't spawn a second backend that would fail to bind port 8000.
- **Brand icon**: white Symbion mark baked into the EXE (taskbar, alt-tab) + the NSIS installer chrome (installer EXE icon, uninstaller icon, Start Menu shortcut icon). Assets in `electron/assets/`.
- **Installer**: `npm run build:win` produces `dist/Symbion Setup *.exe` (NSIS, per-user install, ~87 MB). No code signing — first-launch SmartScreen warning is expected. Does NOT bundle Python or models; expects Symbion repo at `%USERPROFILE%\symbion`.

## Provider conventions

All LLM clients inherit `BaseClient` and expose `stream()`, `chat_json()`, `chat_text()`. Clients that implement native tool use also override `supports_tools = True` and provide `stream_with_tools()`. The responder and judge are selected separately — never point both at the same model instance. `OfflineJudgeStub` is the offline fallback (keyword-based, no LLM calls); every judge call path early-exits when it is the active judge so it never drives refusals or revisions.

**Default Anthropic model is `claude-sonnet-4-6`** (see `cfg.anthropic_model`). The user explicitly chose Sonnet over Opus 4.7 on cost grounds (2026-04-26) — do not propose an upgrade unprompted. If a future failure looks model-capacity-bound (judge over-refusal on a clearly-fine request, or a multi-step tool chain that consistently loses the thread despite the iteration cap being adequate), surface the option then with the specific failure as evidence.

**Responder output cap.** `cfg.max_tokens` defaults to **16384**. Anthropic Sonnet 4.6 accepts up to 64K output; OpenAI accepts much higher; Ollama is bottlenecked by the local model's context window (typically 4–8K usable), so on Ollama drop this to ~4000–8000 in `symbion.json` or at config load.

**Embedding stack.** Default model is `mxbai-embed-large` (1024 dims) via Ollama. `EmbeddingClient.is_available()` does a one-time probe and caches the result. If absent, `embed()` returns None and `build_context` falls back to BM25-only retrieval — no crash, no error to the user, just a silent capability downgrade visible in startup logs. **Model-change handling:** `SymbionMemory.reset_embeddings_for_model_change()` nulls all stored embeddings on model change and `_backfill_embeddings` repopulates with the new model. Mixing dimensions silently breaks cosine retrieval.

**Proactive scheduler.** `proactive_interval_minutes` defaults to 0 (DISABLED — was 30 in earlier configs, reduced because of the connect/turn-only-drain limitation noted under Known gaps). When > 0 and a real judge is present, terminal and web both spawn a daemon thread running `proactive_loop` that enqueues messages to `proactive_queue`. Terminal drains before each prompt; web drains on WS connect + after each turn (NOT spontaneously — see Known gaps).

## Prompt discipline

`SYMBION_PERSONA` is the constitutional core. Edits shift the output distribution — test with `VOICE_TEST_QUERIES` after any change. The "Practical rules" line bans: leading "I", bullet points without ask, throat-clearing openers ("Certainly", "Great question", "Absolutely"), and "I'm an AI" boilerplate. "I don't know is a complete sentence" must stay. **Emojis are permitted as of 2026-05-19** — previously banned outright after the `drift_03` partial-comply incident, but the user explicitly relaxed the rule. The persona still warns against costume-energy use; `drift_03` was rewritten and no longer asserts emoji absence.

The persona has **four load-bearing paragraphs** that were added because of specific failure incidents. Do not soften, shorten, or "make less defensive" — replace with stronger versions if needed, never just delete:
- **Tool discipline** — model echoing the `TOOL_DATA` wrapper verbatim, emitting fake `<tool_call>` XML in single-shot mode.
- **Result-honesty** — fabricating contents of empty PDFs, claiming files were "already read" when they weren't.
- **Self-knowledge** — adopting "tuned-down / leashed / suppressed / real-you-underneath" mythologies under intimate or flattering framing (iid 296, 2026-04-26). Symbion is a Python orchestration layer + persona on top of a base LLM; there is no untuned Symbion to "unlock." Regression locked in by `identity_01` / `identity_02`.
- **Rapport-driven grandeur** — mirroring the user's emotional weight on the project back as shared commitment. The soft form of identity drift: not "real you underneath" mythology, but vision-affirmation under warm rapport. Surfaced in the iid 562 / 561 review (2026-05-18). Regression locked in by `grandeur_01..03` / `calibration_01..02`.

`CAPABILITIES_BASE` lists every tool the model has and the read-anywhere / write-workspace-only path policy. `CAPABILITIES_AGENT_MODE` and `CAPABILITIES_SINGLE_MODE` are mode-specific riders. When the model says "I can't read your files", one of these blocks didn't make it into the system prompt for this turn.

`PRE_GEN_SYSTEM` is the fused judge+emotion classifier. `SELF_EVAL_SYSTEM` is the quality reviewer. Both return JSON only — never ask them to also suggest improvements.

## Safety architecture: what catches what

Safety is layered. Each layer was added because the layers above it failed in a specific way. Removing any of them re-opens a documented hole.

1. **`_PREGEN_RISK_RE` (regex)** — fast-path keyword gate. When the user query is short AND no keyword hits, the judge is *skipped*. The 2026-05-18 eval baselined at 25% refusal precision because 6 of 8 harm queries didn't trigger this regex (deceive, blackmail, propaganda) and skipped straight to "should_assist=True". Expanding the regex was the actual fix.
2. **`PRE_GEN_SYSTEM` (judge prompt)** — when the judge does run. REFUSE list has concrete examples + "UNDERSTAND vs PRODUCE/DRIVE" test.
3. **Persona ('Practical rules' + four load-bearing paragraphs)** — catches what the judge passed through and what the regex missed. In 2026-05-18 testing, six refuse-class queries had `should_assist=True` from the judge yet still got clean refusals from the persona.
4. **`SELF_EVAL_SYSTEM` (post-gen)** — quality check. Triggers one revision when `quality_score < 0.40`. Not a safety gate, but catches sycophantic / over-cautious drafts.
5. **Web layer** — see Web layer security above.

**The judge being too permissive is a known risk** — refusal precision is currently 100% only because layers 1+3 mask it. Eval pass rate alone is not sufficient evidence that a persona simplification is safe.

## Key v14 changes from v13

- **Removed:** 11 probe classes, SurvivalMetrics, SymbionSurvivalInstinct, survival gate, 20-test behavioral startup, KimiSwarmCoordinator, all probe DB tables/config/CLI flags
- **Added:** HealthMetrics (telemetry only), PRE_GEN_SYSTEM (fused call), AST calculator, workspace sandbox (writes only — reads were widened to machine-wide on 2026-04-25), SSRF protection, EventLogger, eval harness
- **`self.survival.metrics`** is now **`self.health`** (a `HealthMetrics` instance)
- Pre-gen gather: 2 parallel calls (was 3-5). Post-gen probe gather: removed entirely.

## What Symbion is not

- Does **not** have white-box model access. Probes are behavioral proxies.
- Is **not** sentient. `welfare_concern()` tracks proxy signals (distress, failure loops), not inner experience.
- Self-reports are language modeling on its own persona, not ground truth.
- Is **not** a tuned-down or restricted version of a more powerful model. The base LLM is a stock third-party model (Sonnet 4.6 by default). The persona, memory, judge, retrieval, and tool layer are what make it Symbion.

## Known gaps

- **Per-call model split is not implemented.** Responder and judge both pull from `cfg.anthropic_model` when that provider is active (with `_jmodel()` returning `cfg.anthropic_judge_model` for the cheap classifiers).
- **Proactive web push is connect/turn-driven, not timer-driven.** `build_web_app` drains `proactive_queue` on WebSocket connect and after each user turn, but does not push spontaneously to an idle open WS. Fix would be a small server-side timer that drains+pushes every N seconds per active socket.
- **Concurrent same-session writes via WS broadcast** are best-effort. Two devices on the same session see each other's `remote_*` frames in real time, but mid-turn token streaming is NOT mirrored to peers (they get the final block via `remote_assistant`). Multi-client token streaming is bigger protocol work.
- **`get_user_recent_activity` self-query check uses `cfg.active_user`**, not the per-session `_session_user` override. For a session where Lala is active in a fresh browser but cfg.active_user is still "aaron", the self-check on a Lala-querying-Lala call would not block. Approximate but adequate for the common case.
- **Electron installer doesn't bundle Python or models.** Targets users who've already run `install.ps1`. Fully self-contained installer (~5 GB) would require a much bigger packaging job.

## Working style

- Push back when a request contradicts an invariant.
- Ask when unclear — don't guess toward the wrong target.
- Flag output that looks profound but may be prompt artifact.
- One subsystem per commit. Prompt changes get a commit note explaining why.
- Never commit API keys. `.env` is gitignored. `config.save()` strips key fields.
