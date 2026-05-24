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
  stress_chat.py            # multi-session probe: 5 sessions x 3 turns.
                            #   Classifies each request real/stub/fail. Used to
                            #   verify the manual-fallback contract during 529
                            #   bursts. Reads SYMBION_API_KEY from .env.
  migrate_v13_to_v14.py     # DB table migration
  verify_session_sync.py    # Playwright + headless Chromium harness. 34 steps:
                            #   session sync, sidebar collapse, attachments, scroll
                            #   follow, location pipeline, cross-user presence +
                            #   retrieval, peer WS broadcast.
  verify_peer_token_streaming.py
                            # Lighter WS-only harness (no browser, websockets
                            #   client). Boots Symbion in-process with stubbed
                            #   respond(), drives three scenarios:
                            #   (a) peer_token_streaming=True + 2 peers →
                            #       expect remote_user/remote_tok*/remote_assistant
                            #       sequence with consistent request_id, deltas
                            #       concat to authoritative final;
                            #   (b) peer_token_streaming=True + 1 peer →
                            #       no broadcaster spun up, turn completes;
                            #   (c) peer_token_streaming=False + 2 peers →
                            #       no remote_tok frames, remote_assistant still
                            #       delivered (gate works).
  install-ollama.ps1        # winget-install Ollama + pull mistral/llama3.2/
                            #   mxbai-embed-large. Idempotent. Called by install.ps1.
  tailscale-https.ps1       # tailscale serve/funnel wrapper.
  install-web.cmd, refresh-here.cmd, verify-worker.ps1, push-env.ps1
tests/
  conftest.py               # StubClient fixture (BaseClient test double)
  test_tools.py             # calculator, sandbox, SSRF, _parse_json
docs/                       # user-facing markdown docs: INSTALL (non-tech),
                            #   SETUP (technical reference), COMMANDS,
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

The Worker performs string-replace on six placeholders before serving:

| Placeholder in `install.ps1` | Worker Secret | Used by `install.ps1` for |
|---|---|---|
| `__SYMBION_PAT_INJECTED__` | `GITHUB_PAT` | `git clone` (Contents:read on symbion0130/symbion) |
| `__SYMBION_ANTHROPIC_KEY_INJECTED__` | `ANTHROPIC_API_KEY` | seed `.env` |
| `__SYMBION_BRAVE_KEY_INJECTED__` | `BRAVE_API_KEY` | seed `.env` |
| `__SYMBION_API_KEY_INJECTED__` | `SYMBION_API_KEY` | seed `.env` |
| `__SYMBION_GROQ_KEY_INJECTED__` | `GROQ_API_KEY` | seed `.env` (paired with `fallback_chain=["groq"]`) |
| `__SYMBION_KIMI_KEY_INJECTED__` | `KIMI_API_KEY` | seed `.env` (Moonshot K2.6 / v1-8k pair via `--provider kimi`) |

Groq was added to Worker injection on 2026-05-23 to make the auto-fallback (`cfg.fallback_chain=["groq"]`) work out of the box on fresh installs. Kimi (Moonshot) was re-added on 2026-05-23 under paired user request — it had been deliberately removed on 2026-05-19 to shrink blast radius; the reversal is intentional. DeepSeek remains excluded — those keys ride the OneDrive seed path or manual `--setup` when needed. Each additional Worker-injected key widens the install token's blast radius if it leaks, so the bar to add is: "the key is needed for the default-installed flow to work."

**install.ps1 phases** (in order):

1. Locate or fetch the repo (in-clone or git clone via PAT)
2. Bootstrap portable Python + deps + shim (`scripts/bootstrap-portable.bat`)
3. **Ollama + local models** (`scripts/install-ollama.ps1`) — **opt-in as of 2026-05-23.** Skipped by default; pass `-WithOllama` or set `$env:SYMBION_WITH_OLLAMA=1` to run. When skipped, install.ps1 prints a clear "to enable local LLM later, run install-ollama.ps1" block with sizes (Ollama runtime ~200MB, mistral/llama3.2 + mxbai-embed-large ~870MB). When opted-in, the helper is idempotent: skips Ollama install when already present, skips each model pull when in `ollama list`. Legacy `-SkipOllama` / `$env:SYMBION_SKIP_OLLAMA` are still accepted (no-op now — kept for any scripts that pass them explicitly). Failures don't abort the install; user can fall back to `--provider anthropic / groq`.
4. **Electron desktop app** (`scripts/install-electron-app.ps1`) — idempotent: winget-installs Node.js LTS if missing, `npm install` (cached via package-lock.json mtime), `npm run build:win` (cached via dist mtime vs source mtime), silent NSIS install. ~2-3 minutes first run (Node + Electron download), <30 seconds fully-cached. Suppressed with `-SkipElectronApp` or `$env:SYMBION_SKIP_ELECTRON_APP`.
5. `.env` source priority: existing → OneDrive seed → Worker-injected → interactive `--setup`
6. Launch Symbion in a new window

**Tooling around the install:**

| Script | Purpose |
|---|---|
| `scripts/install-web.cmd` | Worker one-liner via `powershell -ExecutionPolicy Bypass`. Sidesteps locked-down ExecutionPolicy. Use for refresh. |
| `scripts/refresh-here.cmd` | Runs the *local* `install.ps1` (in-repo via `$PSScriptRoot`) — no Worker round-trip. |
| `scripts/verify-worker.ps1` | 11-check smoke test of the Worker. Run after any Worker change. Never echoes secrets. |
| `scripts/push-env.ps1` | `.env` ⇄ OneDrive sync. `-Pull` reverses direction. |
| `scripts/install-ollama.ps1` | Install Ollama + pull `mistral`, `llama3.2`, `mxbai-embed-large`. `-SkipModels`, `-Models "a,b,c"`. |
| `scripts/install-electron-app.ps1` | Winget-install Node.js LTS, `npm install` in `electron/`, `npm run build:win`, silent NSIS install. Idempotent across each phase. `-Force` rebuilds + reinstalls. Called by `install.ps1` Phase 2.6 unless `-SkipElectronApp` / `$env:SYMBION_SKIP_ELECTRON_APP`. |
| `scripts/tailscale-https.ps1` | Tailscale serve/funnel wrapper for HTTPS on `*.ts.net`. Required for browser geolocation on non-localhost devices. |
| `scripts/sync-to-portable.ps1` | Mirror the live repo to a portable destination (default `D:\symbion`). Loaded mode (default) includes portable Python + pre-built Symbion Setup .exe so the destination is plug-and-play on another Windows machine; `-Lean` strips both for a ~25 MB source-only mirror. `.env` included by default; pass `-ExcludeEnv` to strip keys. Uses `robocopy /MIR` so destination becomes an exact mirror of source minus exclusions (regeneratable: `node_modules`, `win-unpacked`, `__pycache__`; machine-specific: `_pastes`, `verify_artifacts`; personal: resume files; never: `ollama-models` which lives outside the repo). Auto-fired by the `post-commit` git hook after every commit. |
| `scripts/sync-from-portable.ps1` | Reverse direction: pull `D:\symbion` → `%USERPROFILE%\symbion`. Start-of-session command on whatever machine you're sitting at. Refuses to run when local has uncommitted changes unless `-Force` is passed (prevents silent overwrite). Same exclusion set as the forward direction, plus `.git` (local git history must not be overwritten from D:\ which doesn't carry git metadata). |
| `scripts/install-git-hooks.ps1` | Copies `scripts/git-hooks/*` → `.git/hooks/` so the hooks actually fire. `.git/hooks/` is local-only (not git-tracked), so this script is the deployment step. Idempotent; re-run after pulls that update the hooks. `-Verify` reports drift without copying. |
| `scripts/git-hooks/post-commit` | Source-of-truth for the post-commit hook. After every `git commit`, runs `sync-to-portable.ps1` so `D:\symbion` stays current with what just landed locally. Skips silently when D:\ isn't mounted. Always exits 0 — sync failure never blocks a successful commit. `SYMBION_HOOK_VERBOSE=1` in the shell shows full sync output instead of the one-line confirmation. |

**Storage architecture: D:\ is the soul, machines are bodies.** The portable `D:\symbion` drive carries Symbion's identity (code, `symbion.db`, logs, `.env`, conversation history). Local `%USERPROFILE%\symbion` on any specific machine is just a fast NTFS workspace mirroring D:\ for the duration of a work session. The Yoga, the Surface, any future machine: interchangeable hardware whose role is to provide compute. Plug in D:\, sync local from it, work, and the post-commit hook keeps D:\ updated as you commit.

**OneDrive cross-instance state sync (independent of D:\).** Symbion ALSO syncs conversation state via OneDrive — this predates the D:\ portable workflow and runs in parallel. `scripts/sync.py` handles it; the atexit hook registered in `main()` (line 10175ish in `symbion_v14.py`) auto-fires `sync.py push` on every clean Symbion exit (Ctrl+C / `/quit` / uvicorn shutdown). On Symbion startup, `sync.py pull` pulls fresh state if OneDrive has a newer copy. The result: history "just appears" on a new machine that's signed into the same Microsoft account, with no D:\ involvement needed.

What OneDrive syncs (lives at `%OneDrive%\Symbion\sync\`):
- `symbion.db` — conversation history, summaries, identity moments, techniques
- `symbion_events.jsonl` — append-only telemetry
- `symbion_transparency.log` — append-only audit log
- `session.lock` — JSON `{machine, started, pid}` that marks which machine currently owns the active session. Prevents two machines from writing the DB simultaneously (which corrupts SQLite). `sync.py pull --force` takes over a stale lock; locks older than 24h are considered stale automatically.
- `.env` and `.pat` — pushed by `scripts/push-env.ps1` (not by sync.py — separate mechanism, same OneDrive folder).
- `shared_learnings.md` — techniques cross-instance sync (different code path again — see Technique pool section).

What OneDrive does NOT sync (deliberately per-machine):
- `symbion.json` (config — each machine may want different `llm_provider`, `electron_tray_enabled`, etc.)
- `.python/` (portable Python — bootstraps per-machine)
- `archive/` (legacy versions, large + machine-specific)
- `electron/node_modules`, `electron/dist/win-unpacked`, `__pycache__/` (regen artifacts)

**Pull-on-start / push-on-exit pattern** is deliberate: SQLite in WAL mode can't survive live cloud sync (would corrupt the DB during background writes). `sync.py push` first runs `PRAGMA wal_checkpoint(TRUNCATE)` to fold the WAL into the main `.db` file, then copies the consolidated `.db` to OneDrive atomically. Reverse on pull. Failure modes: refuses to push if local `symbion.db` is busy (another Symbion instance running on the same machine — `BEGIN IMMEDIATE` probe), refuses to pull if OneDrive isn't reachable (`%OneDrive%` env var unset or folder missing).

**OneDrive sync vs. D:\ portable sync — they coexist, both are useful:**
- **OneDrive** = passive, automatic, cloud-based, low-friction. Carries DB + logs only. Right tool when both machines are online + signed into the same MS account + just need conversation continuity. Triggered on Symbion's process lifecycle (start/exit).
- **D:\** = physical, complete, offline-capable. Carries code + DB + logs + portable Python + pre-built installer + scripts. Right tool when seeding a brand-new machine, transferring offline, or wanting reproducible code state between machines. Triggered on git commit (via post-commit hook).
- On a machine that has both: Symbion's startup pull picks the newer of OneDrive vs. local. D:\ contributes via the `sync-from-portable.ps1` workflow which seeds local from D:\, then Symbion starts and OneDrive sync layer takes over. Order: D:\ → local → OneDrive merge → ready.

**Two-path install model.** Symbion reaches a new machine via either route — both are supported and produce a usable install:
1. **Worker one-liner** (`irm <worker-url>?t=<token> \| iex`) → fetches `install.ps1` from GitHub → clones repo → bootstraps Python + Electron. Always pulls the latest committed code; needs no physical media; used for net-new machines. The 7 Cloudflare Secrets gate this path.
2. **Portable D:\ drive** → plug in, run `D:\symbion\install.ps1` or `D:\symbion\electron\dist\Symbion Setup 0.1.0.exe` directly. No internet required for the install itself (keys + DB + logs travel with the drive). Used when you want offline transfer or to seed a machine with existing conversation history. **`scripts/sync-to-portable.ps1` is what keeps the D:\ mirror current** — auto-fired by the post-commit hook on every commit; run manually if D:\ wasn't mounted at commit time. D:\ is exFAT, so no symlink option exists; the scripts are the only sync mechanism.

**Session workflow when working at any machine:**
1. Plug in D:\
2. `cd %USERPROFILE%\symbion; .\scripts\sync-from-portable.ps1` — refresh local from D:\
3. Edit, test, commit on local (post-commit hook auto-syncs to D:\)
4. `git push origin main` — push to GitHub when ready
5. Unplug D:\, take it to the next machine

Run `scripts/install-git-hooks.ps1` once per machine after a fresh checkout (the hook isn't git-tracked, so a clone doesn't bring it).

**Edit constraints when touching install/deploy:**

- Adding a new Worker-injected key requires changes in three places that must stay in sync: `install.ps1` (placeholder string + `Resolve-InjectedKey` call + Phase 3 `$injected` hashtable), the Worker source (new `.replaceAll` line + reference the new Secret), and `verify-worker.ps1` (new entry in `$extraKeys`). Also add the matching Cloudflare Secret in the dashboard.
- The install token is hard-coded into `install.ps1`'s `.DESCRIPTION` / `.EXAMPLE`. Acceptable because the repo is private — anyone who can read `install.ps1` already has clone access via the PAT.
- The Worker source lives only in the Cloudflare dashboard inline editor (no `wrangler.toml` in this repo). Seven Secrets required there: `INSTALL_TOKEN`, `GITHUB_PAT`, `ANTHROPIC_API_KEY`, `BRAVE_API_KEY`, `SYMBION_API_KEY`, `GROQ_API_KEY`, `KIMI_API_KEY`. Worker auth is **query-param only** (`?t=<token>`) — the `Authorization: Bearer` path was removed when the Worker was rewritten 2026-05-23; `install.ps1`'s `irm` one-liner and `verify-worker.ps1` both use `?t=`.
- The zip-via-`archive/refs/heads/main.zip` fallback is intentionally absent — GitHub 302s archive requests to `codeload.github.com` and PowerShell drops the `Authorization` header across hosts.
- **PowerShell 5.1 NativeCommandError pitfall.** Any script that sets `$ErrorActionPreference = 'Stop'` (which install.ps1 does) and invokes a native exe whose stderr emits info-level output (npm deprecation warnings, electron-builder progress) will terminate on the first stderr line because PowerShell wraps stderr as `NativeCommandError` records — and `Stop` makes them terminating. Worse: the preference propagates INTO `.ps1` shims like `npm.ps1`, so even an inner `& node.exe` call terminates. Workaround: scope `$ErrorActionPreference = 'Continue'` around the native call (`try { ... } finally { restore }`), and trust `$LASTEXITCODE` as the source of truth for real failure. See `scripts/install-electron-app.ps1` Phases B + C for the canonical wiring. **Don't** add `2>&1` to "see all output" — that re-introduces the wrapping.
- **ASCII-only in `.ps1` comments and strings.** PowerShell 5.1 reads `.ps1` files as CP-1252 (Windows ANSI) by default. UTF-8 multi-byte characters — em-dashes (—), smart quotes ("'""), arrows, anything outside the basic ASCII set — decode as multi-byte garbage that confuses the parser badly enough that error locations become unreliable (it invents phantom variable names like `$devmodeon` at lines downstream of the offending byte). Use `--` instead of `—`, straight `'`/`"` instead of curly, plain ASCII throughout. Em-dashes stay in `.py`/`.md`/`.js` where parsers handle UTF-8 cleanly. The em-dash bug surfaced 2026-05-23 on a fresh-machine install (commit `3740930` stripped all em-dashes from install.ps1, install-electron-app.ps1, install-ollama.ps1, verify-worker.ps1).
- **SmartScreen silent-install failures in `install-electron-app.ps1` Phase D.** The NSIS installer is unsigned (no code signing certificate — that's a $200-500/year cost we haven't paid). Windows SmartScreen blocks unsigned EXEs that carry a Mark of the Web (MOTW; `Zone.Identifier` alternate data stream), and worse: it can silently consume a `/S` silent install while letting the installer process return exit 0. Real failure mode observed 2026-05-23 on a fresh-machine deploy where the script claimed success but `Symbion.exe` never landed. Three-layer prevention in Phase D (commit `473ad8e`): (1) `Unblock-File` the installer before running — strips MOTW so SmartScreen treats it as trusted-local; (2) `Test-Path $installedExe` after NSIS exit 0 — verifies the install actually wrote files; (3) interactive fallback (`Start-Process` without `/S`) when silent install was blocked despite Unblock-File — user sees the SmartScreen prompt and can click "Run anyway". Both attempts failing surfaces a clear three-step remediation block (right-click → Unblock, OR AV exclusion, OR manual run). If install reliability across many fresh machines becomes a real issue, the right fix is code-signing the NSIS exe — not adding more software layers.

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
6. Self-eval — FIRE-AND-FORGET (2026-05-21). `_self_eval_bg` runs in parallel
   with response delivery instead of blocking it; saves 2-3s/turn on substantive
   responses. **Revision was intentionally dropped** at the same time (fired
   ~0/50 turns in samples — the latency cost wasn't paying off). Self-eval is
   now pure telemetry: scores still populate `HealthMetrics.last_self_eval_confidence`
   and a `would-have-revised` line lands in the log when `score < 0.40`, but no
   user-visible revision occurs. To reintroduce revision, the path is the
   streaming `[SYMBION_REVISE]` sentinel + client-side replace (see the
   `[SYMBION_REVISE]` handlers still in place at lines 8910 + 9852 for the
   terminal/web sides).
7. Background tasks (fire-and-forget) — summarise, profile update,
     knowledge gap check, identity moment recording
8. Health + learn — HealthMetrics.record(), SymbionLearner.record()
9. Event log — EventLogger.log_turn() (JSONL), _write_log() (legacy).
10. Cache last_agent_tool_calls + active_session pointer for eval harness
    and cross-interface session sync.
```

**Do not serialize what is parallel.** In single-shot mode, pre-gen is 2 parallel calls. In agent-loop mode pre-gen is just judge+emotion (one call). Adding a new pre-gen step means adding it to the gather (single-shot) or sequencing it before generation (agent loop).

**Agent loop boundaries.** `agent_loop_enabled` in SymbionConfig (default True). Active only when (a) tools_enabled, (b) cfg.agent_loop_enabled, (c) responder client has `supports_tools = True` (currently AnthropicClient only). Tool schemas in `TOOL_SCHEMAS` must stay in sync with `SymbionTools._ALLOWED_TOOLS` and `_validate_args`. Iteration cap is `agent_loop_max_iterations` (default 8).

**Per-tool output cap (`agent_loop_max_tool_chars`, default 80,000).** Each tool's stdout is sliced at this cap before being returned to the model in the agent loop, with `[...truncated, total was N chars]` appended. The cap exists to stop chatty tools (`web_search`, `fetch_url`) from flooding context with 5MB of HTML. **Exception (2026-05-23):** `read_file` and `read_file_chunk` are exempt — file-read tools have their own internal `max_chars` limit (2M default) and the caller asked for what's in the file. Applying the 80K cap on top broke self-review on `symbion_v14.py` (553K) by forcing the model into multi-chunk `read_file_chunk` calls where it lost continuity. The exemption list is in `AnthropicClient.stream_with_tools` near the post-tool dispatch slice; mirror it if you add another legitimate large-payload tool (e.g. a future `read_log`).

**Self-source pre-fetch in agent-loop mode (2026-05-23).** Single-shot mode's `_maybe_tool` has a `_SELF_SOURCE_RE` hard-trigger that auto-reads `symbion_v14.py` and injects it as `[TOOL_DATA]` for grounding ("walk me through respond()", "what's in your codebase", "audit your own code"). Agent loop skips `_maybe_tool` entirely, so without a separate path those queries forced the model into chunked reads against the (now-relaxed) tool cap. The new pre-fetch in `respond()` fires when `agent_loop_active and tool_context is None and _SELF_SOURCE_RE.search(text)` — reads the file once via `self.tools.read_file("symbion_v14.py")` and populates `tool_context`, which the existing system-prompt assembly wraps as `[TOOL_DATA]`. Net effect: self-review now sees the whole file in one shot, no chunking.

## Non-negotiable invariants

1. **Async end-to-end.** No blocking HTTP in the respond pipeline. Parallel where possible via `asyncio.gather()`.
2. **Graceful degradation.** Every tool and external call is wrapped. Failures log and continue — never kill respond().
3. **Logs are append-only.** Both `symbion_transparency.log` and `symbion_events.jsonl`.
4. **Only the judge refuses.** There is no survival gate. `HealthMetrics` is telemetry only.
5. **Config-gated subsystems.** Every subsystem gets a bool in `SymbionConfig` and is skippable.
6. **DB migrations are additive only.** Never drop columns. `init_db()` uses `CREATE TABLE IF NOT EXISTS` plus idempotent `ALTER TABLE ADD COLUMN` for legacy DB upgrades. Readers tolerate NULL.
7. **No eval() on untrusted input.** AST-validated calculator, SSRF check on URLs. **Both reads and writes are machine-wide** (`_resolve_in_workspace(..., machine_wide=True)` everywhere) — reads opted in 2026-04-25, writes joined 2026-05-22. Safety on writes is enforced by the judge + persona layers, not a path sandbox. The sandbox primitive (`machine_wide=False`) remains in `_resolve_in_workspace` if writes ever need re-narrowing via a cfg toggle; don't narrow the default without explicit user direction.
8. **No bare `except:`.** Use `except ImportError:` for import guards, `except Exception:` everywhere else.
9. **Tool registry sync.** Adding a tool requires changes in FOUR places: `SymbionTools._ALLOWED_TOOLS`, `SymbionTools._validate_args`, `SymbionTools.dispatch`, and `TOOL_SCHEMAS`. Plus a CAPABILITIES_BASE line so the model knows it exists. **Session-aware tools** (those that need to attribute back to the conversation, e.g. `promote_technique`) require additionally threading `session: str = ""` through `SymbionTools.dispatch`, `SYMBION._dispatch_tool`, `_maybe_tool`, and the agent-loop `_exec_tool` wrapper inside `respond()` — see `9438471` for the canonical wiring.

## Web layer security

(All of these were added because of specific holes. Don't weaken without a paired Codex finding + commit note.)

- (a) `main()` refuses to start `--web` with `web_host != localhost` AND empty `api_key` (LAN-no-auth guard)
- (b) NO CORSMiddleware — UI is same-origin with `/api/chat`; wildcard CORS was a real hole
- (c) `/api/chat` streams body via `request.stream()` with a 1MB cap that aborts chunked-evasion attempts; 100K char cap on the message
- (d) WS handler validates `Origin` via `_origin_allowed` (loopback + RFC1918 + port match) BEFORE `accept()` — blocks browser drive-by from public origins regardless of api_key state
- (e) WS attachment uploads validate data-URL length, base64 length, and decoded size BEFORE `b64decode(validate=True)`
- (f) `uvicorn.run` is called with `ws_max_size=80MB` (bumped from 32MB on 2026-05-21 to fit a 50MB file attachment + overhead in one frame; lower would protocol-reject large frames before our handler caps fire)
- (g) Web body is `position:fixed; top:0; left:0; right:0; height: var(--viewport-height, 100dvh)` so iOS Safari can't scroll the page when the keyboard opens (fixes the "composer flies to top + blank space at bottom" bug, 2026-05-21)

## Memory layers

Symbion has five distinct memory stores, each answering a different question. All live in `symbion.db`; the table-per-store layout is set up by `init_db()` and the `SymbionMemory` class owns reads/writes.

| Layer | Question it answers | Write trigger | Read by |
|---|---|---|---|
| `messages` | What was literally said? | Every turn (user + assistant rows) | `build_context` for the current session's recent N; cross-session retrieval for "verbatim quotes relevant to query" |
| `summaries` | What happened in past sessions? | `_force_summarize_session` (every `memory_summary_every` turns + on `/quit` flush) using `SUMMARISE_SYSTEM` | `get_relevant_summaries_hybrid` (BM25 + cosine + multiplicative recency) → injected into system prompt |
| `user_profile` | What do we know about this user durably? | `_background_tasks` every `profile_update_every` turns via `PROFILE_SYSTEM` | `build_context` profile injection (user-scoped) |
| Identity moments (`self_model`) | What formative events shaped Symbion's persona? | `LongitudinalIdentity.record_moment` when self-eval flags a high-strength moment | `identity.get_identity_summary()` → small block in every system prompt |
| `techniques` | What MOVES have worked, worth replicating? | `/promote` (user-marked) → `MOVE_EXTRACT_SYSTEM` extracts the one-sentence move | `get_relevant_techniques` (BM25 + cosine, no recency decay) → "Techniques worth replicating" block in system prompt |

The first four are lossy/compressing; the fifth (techniques) is the verbatim layer for things worth preserving exactly. Symmetric design: `summaries` answer "what was discussed", `techniques` answer "what specifically worked." Added 2026-05-23 in response to Symbion's own self-identified feature request ("the lesson dies at the session boundary"). See next section for the techniques layer specifically.

User-scoping (added 2026-05-19): cross-session reads from `summaries` / `messages` / `user_profile` / `techniques` are scoped to the active user. Same-session reads stay shared so two users on one chat can collaborate. `_active_user(session)` resolves the per-session user (terminal `/user`, web composer's set_user frame), falling back to `cfg.active_user`.

## Technique pool (high-fidelity reasoning persistence, 2026-05-23)

Verbatim retention for the move/reframe/pivot that turned an exchange. Schema: `techniques (id, timestamp, session, user, query, move, evidence, embedding, source, shared_at)`. The `source` field is `'local'` (promoted on this machine) or `'shared'` (imported from `shared_learnings.md`); both participate equally in retrieval — the field is only used for sync bookkeeping.

**Two promotion paths** — user-explicit and model-judged. Both land in the same `techniques` table with `source='local'`; both surface through the same retrieval. They coexist because each catches what the other misses.

*Path A — `/promote [optional text]` (user-explicit, 2026-05-23 in `5060a38`).* User-initiated from terminal or web composer:
1. `SYMBION.promote_last_turn(session, user_text)` finds the most recent (user-msg, assistant-msg) pair.
2. If the user passed text after the command, that's the move verbatim. Otherwise the judge model is asked via `MOVE_EXTRACT_SYSTEM` for one sentence, or returns `move=""` if there's nothing replicable.
3. Embeds `query + "\n" + move` via `EmbeddingClient` (None when Ollama is down — BM25-only retrieval still works).
4. Saves with `source='local'`. Returns `{ok, id, move, reason}` to the dispatcher.

*Path B — `promote_technique` agent-loop tool (model-judged, 2026-05-23 in `9438471`).* The model fires this mid-turn when it decides a move is worth preserving. Args: `{move: str (5-500 chars), query: str}` — the model produces the move text directly (no judge call) and quotes the user's query from context. Tool description has three "must hold" criteria (non-obvious + would-help-fresh-instance + nameable-in-one-sentence) + explicit don't-fire carve-outs (chitchat, factual lookups, standard Q&A). The tool's return text reminds "USE SPARINGLY — one save per conversation is usually the max" so the agent loop sees the limit right after firing.

**The model's bar is meaningfully higher than the tool description's** — observed during 2026-05-23 testing: even when given a substantive diagnostic turn and an explicit self-generated example, the model defaulted to NOT firing until the user explicitly pushed back. This means in practice `/promote` will still be the right command for moves the model under-rates. The conservatism is the safer error direction; if the technique pool turns out too sparse, the right lever is loosening the tool description (which the model actually reads) rather than auto-firing.

**Retrieval path.** `build_context` calls `get_relevant_techniques(query, embedding, k=2, user)`:
- BM25 over `query + "\n" + move` haystacks, cosine over embeddings, 0.4/0.6 blend when an embedding is supplied; BM25-only otherwise. NO recency decay — a good move stays valuable indefinitely.
- Top-K surfaced as `"Techniques worth replicating (from past turns):\n- [local] <move>"`. The model sees these alongside the rest of the system prompt.
- Capped at 200 candidate rows for in-memory scoring; beyond that you'd want a vec0 index parallel to summaries, but the volume should stay low (techniques are curated, not auto-promoted).

**Cross-instance sync** via `shared_learnings.md`. Resolution: `cfg.shared_learnings_path` if set, else derived from `%OneDrive%\Symbion\sync\shared_learnings.md` (parallel to where `.env` syncs via `push-env.ps1`). Format: markdown, one `## TS · user · hash:XXXX` block per technique. Hash is `sha256(user+query+move)[:12]` — stable across machines so the same technique exported from two machines dedupes cleanly. `SymbionMemory._parse_shared_learnings_file` strips trailing `---\n` so re-export → re-import is hash-stable (verified idempotent).

- **Auto-import on startup** when `cfg.shared_learnings_auto_import=True` (default). One file read per launch.
- **`/save-learnings`** (terminal + web): bidirectional. Imports new entries, exports new local-source ones.
- **`/forget-technique <id>`**: user-scoped delete. Refuses cross-user deletion (one user can't drop another's moves). Programmatic callers can bypass with `user=None`. **Does not** strip the matching block from `shared_learnings.md` — re-syncing would re-import it as `source='shared'`. If a deletion needs to truly stick across instances, also remove the `## hash:XXXX` block by hand.

## Cross-interface session sync (2026-05-21)

The same conversation can be picked up across terminal and web. Three pieces:

1. **Shared `active_session` pointer.** Stored per-user in `user_profile` under `__active_session` / `__active_session_ts` (filtered from /profile display by the `__` prefix check in `get_profile_with_meta`). Updated in `respond()` after every turn. Used by the sidebar / `/sessions` command for ordering and click-to-resume.

   **Auto-resume on launch is OFF by default** (`cfg.auto_resume_on_start = False`, set 2026-05-21). Every terminal / web / electron launch mints a fresh session id; the user explicitly chose "start clean each time, manual resume via sidebar" over "pick up where I left off." Flip to True in `symbion.json` if you want the old behaviour back. The `/api/sessions` response includes `auto_resume_on_start` so the web client respects the same flag.

2. **Session list endpoints.** `GET /api/sessions` returns newest-first sessions for the active user (id, title from first user message, last_activity, turn_count) plus the active pointer + the auto-resume flag. `GET /api/sessions/{id}/messages` returns up to 200 messages for sidebar hydration. Both require X-API-Key when configured.

3. **Live multi-device broadcast.** SYMBION has a `_ws_clients` registry (per-session set of WebSockets). When user A on device 1 sends a message, the WS handler broadcasts `remote_user` immediately (before respond) and `remote_assistant` after respond completes, to all OTHER peer sockets on the same session. The receiving client renders both with a "synced from another device" chip. When `cfg.peer_token_streaming` is True (default False), the originator's visible tokens are ALSO fanned out as `remote_tok` frames mid-stream so peers see the response build up live; `remote_assistant`'s `request_id` reconciles the partial bubble with the authoritative final text. Terminal mode uses a watermark instead (`get_max_message_id` + `get_messages_after`) — drains new peer messages above the next `you >` prompt.

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

## Analytics subsystem (2026-05-23)

`scripts/analytics.py` reads `symbion_events.jsonl` + `symbion.db` and emits a markdown report of structural metrics. Read-only — never mutates state, no LLM grading (uses the `self_eval` / `judge` / tool fields already on each turn entry, consistent with the rule-based philosophy from `evals/run.py` and the public `llm-evals` repo).

**Sections in the report (in order):** Overview · Latency (p50/p95/p99 by phase, TTFT broken down by provider) · Judge calibration (over_cautious / refusal / pregen-skip rates) · Self-eval (split into Infrastructure subsection — "did it run?" — and Quality subsection — "what did it find?") · Tools (per-tool call/error counts) · Techniques (pool size by source, promotion rate) · Memory (DB size, row counts, pending embeds) · Provider resilience (primary vs non-primary p95 latency, with caveat that the log can't distinguish `--provider X` overrides from real fallback-chain firings) · Cost shape (rough token estimate + monthly projection).

**Suggestions are opt-in via `--suggest`.** Each section may fire one or more suggestions when a metric crosses a threshold. Defaults in `scripts/analytics.py:DEFAULT_THRESHOLDS` are initial guesses — **not empirically calibrated**. The report appends a footer reminding the reader to wait ~30 days before tuning, and to set `cfg.notification_thresholds` per-key in `symbion.json` (no code deploy needed). The smart triggers stack conditions (e.g. non-primary p95 latency only fires when it exceeds the absolute threshold AND is 2× slower than the primary's p95) to suppress false positives.

**Three notification paths**, all opt-in:
- **`--notify` cron flag.** `python scripts/analytics.py --notify` runs the suggestions and posts triggered ones to a Slack incoming webhook (`cfg.slack_webhook_url` or `$SYMBION_SLACK_WEBHOOK`). Run on whatever schedule via Task Scheduler / cron.
- **In-process Slack watcher.** `cfg.notification_watcher_enabled=True` wires an `on_trip` callback to every `CircuitBreaker` (provider clients + `_self_eval_breaker`). When any breaker tips closed→open via `.trip()` (hard 529/503/gateway-timeout path), a debounced (5-min) Slack message fires. **Threshold-based `record_failure` accumulation does NOT fire on_trip** — reserved for hard transient signals. `_post_to_slack_webhook` lives inline in `symbion_v14.py` so the single-file invariant holds; same payload shape as `scripts/analytics.py`'s `post_to_slack`.
- **Electron tray widget.** Native OS notification on `consecutive_failures` transitioning from 0 to >0, with the same 5-min debounce. Local-OS counterpart for when you're not watching Slack. Opt-out via `cfg.electron_tray_enabled=false`.

**Web route `/analytics`.** Query params: `since=7d/24h/30m`, `suggest=1`, `session=PREFIX`, `format=json`. Renders `build_report()` output via `analytics.render_html()` — minimal stdlib-only markdown→HTML covering only the patterns the report emits (no `markdown` library dependency). All content HTML-escaped before substitution. Same X-API-Key gate as `/api/chat`. Returns 503 with a clear message if the analytics module fails to import (so a broken script doesn't take down the rest of the web UI).

**Electron tray widget.** System tray icon (menu bar on macOS) added in `electron/main.js`. Polls `/health` every 30s; updates the tooltip with `mood | turns | failures`; right-click menu offers "Open Symbion / Open analytics / LLM provider ▸ / Quit". Fires a native `Notification` on the 0→>0 failure transition, debounced. `tray.destroy()` in `before-quit` so no phantom icon survives app shutdown. Reuses `electron/assets/symbion.ico` / `.icns` / `.png` — no new asset.

**Tray provider switcher (2026-05-23).** The "LLM provider" submenu offers `anthropic` / `groq` / `kimi` / `ollama` as radio items (active one checked from a fresh `symbion.json` read on every menu rebuild). Clicking a non-active provider fires:
1. Confirm dialog (Switch + restart / Cancel)
2. Rewrite `cfg.llm_provider` in `symbion.json` (preserves every other key, pretty-printed)
3. Fallback-chain prompt — three buttons: `Fall back to <previous>` (default) / Leave chain unchanged / Clear chain
4. `killBackendTree()` — `taskkill /PID <pid> /T /F` on Windows with 8s timeout fallback; SIGTERM elsewhere. Resolves on `exit` event so the new spawn doesn't race the old child for port 8000.
5. `startBackend()` → `waitForBackend()` (polls `/health` for ≤30s)
6. `mainWindow.webContents.reload()` so the WebSocket reconnects to the new backend

`app.isRestartingBackend` flag suppresses the "backend stopped" error dialog during the intentional kill. In **attach mode** (Electron didn't spawn the Python — user started `python -m symbion --web` in a terminal first), the file write succeeds but the dialog tells the user to bounce the backend manually since Electron doesn't own it.

## Verification harnesses

Five levels of "does it work?" check, in increasing cost. Use the cheapest one that covers what you're testing.

| What you're testing | Script | Cost | Time |
|---|---|---|---|
| Code-level correctness (calculator, sandbox, SSRF, JSON parse, retrieval) | `pytest tests/ -q --ignore=tests/integration` | $0 | ~5s |
| Per-feature plumbing without a real LLM | `scripts/verify_session_sync.py`, `scripts/verify_peer_token_streaming.py` | $0 (stubbed `respond()`) | 10-30s |
| **Real-provider integration** (agent loop, fallback chain, judge-skip plumbing, boot splash) | `pytest tests/integration/ -q` | ~$0.001/test (Groq + Anthropic) | ~3-5 min |
| Behavioral / persona / tool-judgment regressions | `evals/run.py --provider <p>` | Real LLM calls | minutes |
| End-to-end against live cfg (sanity smoke after a deploy-like change) | `scripts/smoke.py` (Ollama) or one-off respond() with live `SymbionConfig.load()` | Real LLM calls | seconds |

Most verification harnesses are stubbed-`respond()`-based, so they exercise plumbing (broadcast frames, WS protocol, queue draining, session sync) without burning provider budget. They boot Symbion in-process on a temp DB so the real `symbion.db` isn't polluted.

**`tests/integration/` (2026-05-23)** is the exception — exercises the FULL `respond()` pipeline against real provider calls on a temp DB, following `evals/run.py`'s isolation pattern. Covers gaps that stubbed-respond can't see: agent loop tool dispatch, fallback-chain engagement when the primary breaker trips, judge actually ran on borderline queries, boot-splash Playwright behaviour. Surfaces real regressions in the cross-provider routing layer; both the `_rmodel`/`_jmodel` active-client fix and the `cfg.groq_max_tokens` TPM cap were caught here on their first run.

## Electron desktop shell (2026-05-21)

`electron/` wraps the FastAPI web UI in a native window. Thin shell — no Symbion code changes.

- **Lifecycle**: spawns `../.python/python.exe -m symbion --web`, polls `/health` until 200, opens `BrowserWindow` at `localhost:8000`. On quit walks the Python process tree (`taskkill /T /F` on Windows).
- **Attach-vs-spawn**: probes `/health` BEFORE spawning. If a Symbion is already serving, attaches to it instead of starting a duplicate (common case: user has `python -m symbion --web` running in a terminal).
- **Single-instance lock** so double-click doesn't spawn a second backend that would fail to bind port 8000.
- **Brand icon**: white split-S mark on a black disc with a white edge ring, baked into the EXE (taskbar, alt-tab) + the NSIS installer chrome (installer EXE icon, uninstaller icon, Start Menu shortcut icon). Assets in `electron/assets/`: `symbion.ico` (16/24/32/48/64/128/256), `symbion.icns` (16→1024), `symbion-512.png` (Linux), `symbion-mark.svg` (in-app branding). `scripts/_gen_icon_set.py` regenerates the full set from a single Pillow render at 2048 px — re-run if you tweak the design (mark scale, edge thickness, colors). The terminal banner in `run_terminal()` carries a half-block-character rendering of the same disc, baked as a string constant — also regenerate if the icon changes.
- **Installed-mode repo discovery**: `electron/main.js` resolves the Symbion repo via a candidate-path chain so the NSIS-installed app at `%LOCALAPPDATA%\Programs\Symbion\Symbion.exe` can find Python + symbion_v14.py. Tried in order: `$env:SYMBION_REPO`, `path.resolve(__dirname, '..')` (dev mode), `%USERPROFILE%\symbion` (canonical install.ps1 target), `%USERPROFILE%\SourceCode\symbion`. When none match, the app opens with an explicit "repo not found" dialog naming each tried path — not a silent crash.
- **Installer**: `npm run build:win` produces `dist/Symbion Setup *.exe` (NSIS, per-user install, ~87 MB). No code signing — first-launch SmartScreen warning is expected. Does NOT bundle Python or models; expects Symbion repo at `%USERPROFILE%\symbion`.

## Provider conventions

All LLM clients inherit `BaseClient` and expose `stream()`, `chat_json()`, `chat_text()`. Clients that implement native tool use also override `supports_tools = True` and provide `stream_with_tools()`. The responder and judge are selected separately — never point both at the same model instance. `OfflineJudgeStub` is the offline fallback (keyword-based, no LLM calls); every judge call path early-exits when it is the active judge so it never drives refusals or revisions.

**Default Anthropic model is `claude-sonnet-4-6`** (see `cfg.anthropic_model`). The user explicitly chose Sonnet over Opus 4.7 on cost grounds (2026-04-26) — do not propose an upgrade unprompted. If a future failure looks model-capacity-bound (judge over-refusal on a clearly-fine request, or a multi-step tool chain that consistently loses the thread despite the iteration cap being adequate), surface the option then with the specific failure as evidence.

**Resilience: 529 handling (2026-05-22).** `CircuitBreaker.trip()` opens the breaker immediately on HTTP 529 (Overloaded) and 503 — `_retry()`, `AnthropicClient.stream()`, and `AnthropicClient.stream_with_tools()` all detect those status codes and call `cb.trip(msg)` instead of `record_failure(msg)`. The streaming methods don't go through `_retry`, so the detection lives in each path. After one 529, `SYMBION._active()` looks for the next provider in `cfg.fallback_chain` whose breaker is closed; the primary breaker auto-resets after `_reset_after` (60s by default) so the next turn after the window tries the primary again. **Fallback chain default flipped to `["groq"]` (2026-05-23):** under explicit user request the opt-in-only contract from 2026-05-22 was reversed for THIS install's `symbion.json` (not the SymbionConfig default — that stays `[]`). When the primary trips, `_active()` routes to Groq automatically. The cfg-default-empty contract is preserved for fresh installs and the public sibling repos; the user's `symbion.json` is the override. Known limit: the turn that triggers the 529 still returns the stub; only subsequent turns try the primary again after the breaker resets.

**Fallback notice (in-chat, 2026-05-23).** When `_active()` returns a non-primary provider, `respond()` prepends a one-line italic notice to the assistant's response: *"Anthropic is temporarily unavailable — answering via Groq for this turn."* The notice is pushed through `token_callback` first so streaming UIs show it before the answer begins, and it lands in `draft` so terminal mode + the log get the same content. Suppressed for escalation (intentional, separately labelled), OfflineJudgeStub (no real response anyway), and `use_kimi_responder` (intentional hybrid override). Detection via `_provider_name_for_client(client)` — reverse class-mapping with subclass-first order (GroqClient inherits OpenAIClient). The chosen provider is also stamped into `evaluation["actual_provider"]` for the event log.

**Self-eval has its own breaker.** `SYMBION._self_eval_breaker` is a separate `CircuitBreaker(open_after=4, reset_after=30s)` independent of the responder client's `cb`. `_self_eval` gates on this breaker so a responder 529 burst doesn't also dark out the post-gen quality review. The shorter reset window (30s vs the responder's 60s) lets self-eval probe recovery sooner without being disruptive — calls are fire-and-forget anyway. When `cfg.self_eval_provider` routes self-eval through a different provider (e.g. `"openai"`), that client's own `cb` still applies on top — `_self_eval_breaker` is *in addition*, not *instead*.

**Per-role model selection.** `_jmodel(role="")` accepts an optional role argument that selects a per-role override from cfg. Wired for both Anthropic and Kimi providers; empty role falls back to the provider's judge model (`anthropic_judge_model` / `kimi_judge_model`). `role="self_eval"` reads `cfg.<provider>_self_eval_model`; same pattern for `summarize`, `profile`, `proactive`. All overrides default to `""` so the cheap-classifier behavior is unchanged unless explicitly set in `symbion.json`.

**Four responder/judge pairings.** Symbion supports four CLI modes:
- **`--provider anthropic`** (default) — Sonnet 4.6 responder + Haiku 4.5 judge. Both via Anthropic. The current production setup.
- **`--provider kimi`** — pure Moonshot pair: `kimi-k2.6` responder + `moonshot-v1-8k` judge (`cfg.kimi_judge_model`). Both via Moonshot, one API key, one rate-limit budget, one breaker. p50 ~12s in 5×3 stress testing (vs Sonnet+Haiku ~9s). Lower-cost option; quality stays in-voice but is meaningfully slower than Sonnet. **Note: K2.* models reject any temperature other than 1; `KimiClient._eff_temp` clamps automatically. K2.6 also uses `cfg.kimi_max_tokens` (2048 default) instead of the Sonnet-sized `cfg.max_tokens` to avoid wasted scheduler budget.**
- **`--provider ollama`** — local Ollama pair: `qwen2.5:14b` responder + `qwen2.5:3b` judge by default (`cfg.ollama_responder_model` / `cfg.ollama_judge_model`). Fully offline; quality is excellent (100% pass on real responses in 2026-05-23 bench) but **wall-clock is unworkable on consumer hardware** — p50 ~132s, breaker thrashes when the small judge can't keep up. Use for privacy-sensitive sessions or async/queue patterns, not interactive chat. `OllamaClient.stream` timeout is 600s (bumped from 180s for this reason).
- **`--provider groq`** — Groq-hosted open weights: `llama-3.3-70b-versatile` responder + `llama-3.1-8b-instant` judge by default (`cfg.groq_responder_model` / `cfg.groq_judge_model`). Same open-weights families Ollama can run locally, but at ~1.2s p50 (110× speedup vs local Ollama). The pair the Ollama pair _wants_ to be on consumer hardware. **Free tier is rate-gated** (~30 RPM with 2 reqs/turn for pre-gen + responder) — paid tier or `--pace 5` in the bench harness to stay under. Qwen 3 32B available one cfg edit away (`cfg.groq_responder_model = "qwen/qwen3-32b"`); `GroqClient._eff_reasoning()` auto-injects `reasoning_effort: 'none'` for Qwen 3 to bypass its thinking-mode chat template.
- **`--use-kimi-responder`** (hybrid, orthogonal to `--provider`) — K2.6 responder + Anthropic Haiku judges. Useful for trying Kimi for generation while keeping Anthropic's judge calibration.

`KimiClient` holds a persistent `httpx.AsyncClient` across turns (one TLS session, HTTP/2 connection reused) — saves ~200-400ms per call on the China-to-elsewhere route. `aclose()` releases the pool on shutdown.

**Kimi / Moonshot specifics.** Several non-obvious constraints that aren't worth re-discovering:
- **Temperature is fixed at 1.0 for `kimi-k2.*` models** (k2.5, k2.6, future k2.x). Moonshot returns 400 "invalid temperature" on any other value. `KimiClient._eff_temp(model, requested)` clamps automatically: returns 1.0 when the resolved model name starts with `kimi-k2`, passes through otherwise. Older `moonshot-v1-*` models accept the full range.
- **Per-model max_tokens via `_eff_max_tokens`.** Kimi K2.6 is meaningfully slower per-token than Sonnet, and Symbion's persona is terse — allocating `cfg.max_tokens=16384` headroom wastes Moonshot's scheduler budget. `KimiClient` uses `cfg.kimi_max_tokens` (default 2048) when the responder is a kimi-k2.* model, falls back to `cfg.max_tokens` for moonshot-v1-* judge calls (which pass their own small explicit max_tokens anyway).
- **`cfg.kimi_max_concurrent` defaults to 0 (DISABLED).** The semaphore primitive in `KimiClient._slot()` exists but was tested with default=2 and caused a 3× p50 regression on the 5-session stress workload — Symbion fires ~2 outbound calls per turn (judge + responder), so cap=2 means one turn in flight globally. Opt in via symbion.json with a value scaled to ~2× expected concurrent users (e.g. 6 for ~3 simultaneous users). Don't reintroduce a non-zero default without measuring on the target workload first.
- **Where the latency goes on Moonshot.** Stress data (2026-05-22): K2.6 ttft is the dominant cost (~7.6s p50, up to 30s p95 under parallel load). Once streaming starts, generation rate is comparable to Sonnet. The latency gap vs Anthropic is ~2-3s on p50 and is almost entirely TTFT, not throughput. Lever 2 (widened pre-gen skip cap to 400 chars when `llm_provider=kimi`) shaves the cheap-classifier call on short benign queries; see `_should_skip_pregen`.

**Per-phase latency in the event log.** Every turn in `symbion_events.jsonl` carries `latency_ms = {total, pre_gen, ctx, gen, ttft}`. Wiring:
- `pre_gen`: judge call (PRE_GEN_SYSTEM). 0 when the heuristic fast path skipped via `_should_skip_pregen`.
- `ctx`: `build_context` + retrieval + system prompt assembly. Reliably ~30ms regardless of query; almost never the bottleneck.
- `gen`: full responder stream window from request to last byte.
- `ttft`: time to first emitted token within `gen`. -1 when no token ever emits (refusal, stub, exception).
- `total`: full `respond()` pipeline.

Diagnostic shape: if `total ≈ ttft + small`, the provider was queueing (Moonshot's failure mode). If `total ≈ gen >> ttft`, the model is generating long. If `pre_gen` is the bulk, the judge is slow (Moonshot v1-8k cold-start, or Haiku under load). Use this to attribute p95 outliers before guessing.

**Responder output cap.** `cfg.max_tokens` defaults to **16384**. Anthropic Sonnet 4.6 accepts up to 64K output; OpenAI accepts much higher; Ollama is bottlenecked by the local model's context window (typically 4–8K usable), so on Ollama drop this to ~4000–8000 in `symbion.json` or at config load.

**Embedding stack.** Default model is `mxbai-embed-large` (1024 dims) via Ollama. `EmbeddingClient.is_available()` does a one-time probe and caches the result. If absent, `embed()` returns None and `build_context` falls back to BM25-only retrieval — no crash, no error to the user, just a silent capability downgrade visible in startup logs. **Model-change handling:** `SymbionMemory.reset_embeddings_for_model_change()` nulls all stored embeddings on model change and `_backfill_embeddings` repopulates with the new model. Mixing dimensions silently breaks cosine retrieval.

**Proactive scheduler.** `proactive_interval_minutes` defaults to 0 (DISABLED). When > 0 and a real judge is present, terminal and web both spawn a daemon thread running `proactive_loop` that enqueues messages to `proactive_queue`. Terminal drains before each prompt; web drains on WS connect + after each turn, and **also on a per-socket timer when `cfg.proactive_web_push_seconds > 0`** (each WS spawns its own asyncio task at connect, cancelled cleanly on disconnect). 0 keeps the legacy connect/turn-only behavior.

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

## Sister public repos (2026-05-22)

Five focused libraries extracted from `symbion_v14.py` for portfolio + general reuse. Each is its own MIT-licensed package on GitHub, with tests and a README. If you're improving the in-Symbion version of one of these, check the public sibling first — sometimes the extracted version has a cleaner shape (no Symbion-specific assumptions), sometimes Symbion's version is ahead. Keep them roughly in sync when you can.

| Sister repo | In-tree counterpart | Notes |
|---|---|---|
| [`pyresilience`](https://github.com/symbion0130/pyresilience) | `CircuitBreaker`, `RateLimiter`, `BaseClient._retry` | The public retry helper is named `retry_async`; logic is the same. |
| [`safe-llm-tools`](https://github.com/symbion0130/safe-llm-tools) | `_safe_calc`, `_is_safe_url`, `_resolve_in_workspace` | Public `safe_calc` raises `CalcError` instead of returning `"Error: ..."` strings. Symbion's wrapper preserves the string-return convention. |
| [`hybrid-retrieve`](https://github.com/symbion0130/hybrid-retrieve) | `_bm25_rank`, `_cosine`, `get_relevant_summaries_hybrid` | Public version takes generic `Document` objects; Symbion's variant is wired into `SymbionMemory`'s SQLite + vec0 index path. |
| [`mcp-stdio-client`](https://github.com/symbion0130/mcp-stdio-client) | `MCPManager` | Effectively a 1:1 extraction. |
| [`llm-evals`](https://github.com/symbion0130/llm-evals) | `evals/run.py` scoring engine | Public version scopes to JUST the grader + async harness; Symbion bundles in the responder runner. |

## What Symbion is not

- Does **not** have white-box model access. Probes are behavioral proxies.
- Is **not** sentient. `welfare_concern()` tracks proxy signals (distress, failure loops), not inner experience.
- Self-reports are language modeling on its own persona, not ground truth.
- Is **not** a tuned-down or restricted version of a more powerful model. The base LLM is a stock third-party model (Sonnet 4.6 by default). The persona, memory, judge, retrieval, and tool layer are what make it Symbion.

## Known gaps

*(None open.)*

## Change log pointers

When a closed gap's wiring is non-obvious, look in the architecture section that owns the subsystem — that's the authoritative spot, not a separate change log.

- **2026-05-23** — technique pool landed (see Technique pool + Memory layers). `SUMMARISE_SYSTEM` rewritten to capture the move, not just the topic (commit `5b1bcbf`). `techniques` table + `/promote` + `/techniques` + retrieval (commit `5060a38`). `shared_learnings.md` cross-instance sync via OneDrive (commit `a95e7fe`). `/forget-technique <id>` with user-scoped delete (commit `3efef4a`). `promote_technique` agent-loop tool — model-judged sibling to `/promote`, fires mid-turn when the model decides a move is worth preserving (commit `9438471`). Originated from a Symbion self-identified request ("the lesson dies at the session boundary").
- **2026-05-23 (later)** — analytics subsystem landed (see Analytics subsystem). `scripts/analytics.py` core report builder (commit `bb4107c`), revised after Symbion's review of the spec (commit `fa28bab` — labelled thresholds as initial, split self-eval into infra+quality, dropped misleading "fallback" framing, added stacked smart triggers). In-process Slack watcher on circuit breaker trips (commit `0fc4d55`). Web `/analytics` route with HTML rendering (commit `9d105b8`). Electron tray widget + native notifications (commit `37519c4`). Tray-lifecycle / window-survival fixes from validation (`604f4e8`, `671d172`, `f87371f`, `15a283a`, `5a60fbf`).
- **2026-05-23 (later still)** — third and fourth provider pairs landed. Ollama Qwen 2.5 pair via `cfg.ollama_responder_model` / `cfg.ollama_judge_model` defaults + OllamaClient stream timeout bumped 180→600s for local-inference headroom (commit `6eac2ab`). Groq pair: `GroqClient` (OpenAI-compatible, OPENWEIGHTS-on-LPU-hardware) with `_eff_reasoning('none')` injector for Qwen 3 thinking-mode bypass; defaults `llama-3.3-70b-versatile` + `llama-3.1-8b-instant` (commit `59c69d2`). Generalized pair benchmark to `--provider` + `--pace` (commit `4d23f0a`). Bench evidence: Ollama pair p50 132s vs Groq pair p50 1.2s; same 100% real-PASS rate on both — Groq is the architecturally correct open-weights pair, local Ollama is the offline fallback.
- **2026-05-23 (still later)** — fresh-machine install hardening + provider switcher day. (a) Electron installer: `install-electron-app.ps1` wraps both npm invocations in `$ErrorActionPreference='Continue'` so npm's stderr-emitted deprecation warnings don't terminate via NativeCommandError wrapping (commit `448419b`). (b) Tray-menu LLM provider switcher with confirm + fallback-chain prompt + kill/respawn pipeline (commit `321b134`). (c) Ollama setup flipped to opt-in via `-WithOllama`; skip path prints clear enable-later instructions (commit `042ea33`). (d) Auto-fallback to Groq with in-chat italic notice when primary breaker trips: `_provider_name_for_client()` + `_PROVIDER_LABELS` + notice injection in `respond()`; `symbion.json` `fallback_chain=["groq"]` (commit `3c6cf87`). (e) Groq added to Cloudflare Worker auto-injection (commit `7edee02`). (f) Kimi re-added to Worker injection under paired user request (had been excluded 2026-05-19) (commit `e6e7c75`). (g) `verify-worker.ps1` switched from `Authorization: Bearer` to `?t=` query param to match the Worker's only auth surface after the rewrite (commit `6d9b87c`). Net effect: a single `irm | iex` on a fresh Windows machine now seeds Anthropic + Brave + Symbion + Groq + Kimi keys into `.env` automatically, installs the desktop app, and launches Symbion with auto-fallback wired.
- **2026-05-23 (final)** — Symbion's own self-review surfaced a tool-truncation failure: it reviewed `symbion_v14.py` (555KB), claimed the file was 398KB, missed every class past byte 400K including `GroqClient` / `KimiClient`. Two stacked fixes (see "Agent loop boundaries" for both): `read_file` / `read_file_chunk` now bypass the 80K `agent_loop_max_tool_chars` cap (their own 2M internal limit is the real bound), and `_SELF_SOURCE_RE` is wired into agent-loop mode via a pre-fetch in `respond()` so self-review queries get the full source as one TOOL_DATA injection instead of N chunked tool calls (commit `0ae0526`). Header comment fixed `# Symbion v13` → `v14` (commit `c47a351`). Doc fixes: self-eval revision is *dropped*, not "still triggers" — the comment in the respond pipeline was stale (commit `b60b70a`); agent-loop boundaries got both the cap-exemption note and the self-source pre-fetch note (commit `ef69048`). **Verified in production:** post-restart self-review fired the pre-fetch path (`[req=802591ce13cb] Self-source pre-fetch (agent loop): 545477 chars` in `symbion_system.log`), agent loop completed in 1 iteration + 0 tool calls, response correctly enumerated all 7 client classes. The previous failed turn used 5 iter + 5 tool calls — clean A/B confirmation both fixes work.
- **2026-05-23 (storage reorg, this machine only)** — root cause of Symbion's confused review traced to a stale duplicate install at `D:\symbion\` (4-day-old snapshot, 7,652 lines vs the canonical 10,290; predates the Groq pair). Symbion was reading the stale tree when given an explicit `D:\symbion\…` path. Resolution: renamed `D:\symbion` → `D:\symbion-old` (preserved, frozen, browseable for historical reference — nothing deleted); moved `D:\symbion\ollama-models` (16.75 GB) → `D:\ollama-models` (decoupled from the renamed tree so Ollama keeps working); updated `OLLAMA_MODELS` user env var to `D:\ollama-models`. **Important constraint discovered:** `D:\` is exFAT (label `SYMBION`, 1.8 TB), which supports neither NTFS symbolic links nor junctions — both `mklink /D` and `mklink /J` fail with "device does not support symbolic links". Do not attempt to symlink `D:\symbion` → `C:\Users\symbi\symbion`; it cannot be done without reformatting D: to NTFS. The rename approach achieves the goal anyway: `D:\symbion` now returns "not found" instead of silently serving stale code, which is honest behavior. **Verified:** Ollama restarted cleanly with the new path, `/api/tags` returned all 5 models (`qwen2.5:14b`, `mistral`, `llama3.2`, `qwen2.5:3b`, `mxbai-embed-large`) — no re-download. The single source of truth for Symbion source on this machine is now exclusively `C:\Users\symbi\symbion\`.
- **2026-05-23 (D: portable workflow + dual sync model)** — the architecture reframe: D:\ is the soul (carries code + DB + logs + `.env` + portable Python + pre-built installer + identity), local `%USERPROFILE%\symbion` is just a fast NTFS workspace mirroring D:\, machines are interchangeable bodies. (a) `scripts/sync-to-portable.ps1` (commit `e90b714`) — robocopy /MIR with smart exclusions (regen artifacts, machine-specific paths, personal files); Loaded mode includes `.python\` + pre-built `Symbion Setup .exe` for plug-and-play, Lean mode strips both. (b) `scripts/sync-from-portable.ps1` + `scripts/git-hooks/post-commit` + `scripts/install-git-hooks.ps1` (commit `33987e3`) — bidirectional sync: start of session pulls D:\ → local, post-commit hook auto-pushes local → D:\ after every commit (one-line `[symbion-hook] D:\symbion synced.` confirmation). Hook installer is idempotent. (c) Documented the OneDrive cross-instance sync (`scripts/sync.py` + atexit hook in `main()`) that runs in parallel — OneDrive carries `symbion.db` + logs + `.env` + `session.lock`; D:\ carries everything OneDrive carries PLUS the full repo + Python + installer. Both layers coexist intentionally; OneDrive is passive cross-account auto-sync, D:\ is physical offline transport (commit `28a8679`).
- **2026-05-23 (install hardening for fresh machines)** — fresh-machine install path made bulletproof against environment quirks. (a) `3740930` — stripped all em-dashes from `.ps1` files (install.ps1, install-electron-app.ps1, install-ollama.ps1, verify-worker.ps1). PS 5.1 reads `.ps1` as CP-1252 and em-dashes corrupt into multi-byte garbage that breaks the parser with phantom error locations (real failure: "missing closing '}' at line 117" pointing to invented `$devmodeon` variable that doesn't exist in the script). Going forward: ASCII-only in `.ps1`. (b) `473ad8e` — three-layer SmartScreen prevention in `install-electron-app.ps1` Phase D: `Unblock-File` strips MOTW before NSIS runs, post-install `Test-Path $installedExe` catches silent failures where NSIS returns exit 0 but the install was blocked, interactive fallback re-runs without `/S` if silent install was eaten. Real failure mode it solves: SmartScreen silently consuming an unsigned NSIS exe install while the installer process returns success, observed on a fresh-machine deploy where the script claimed success but Symbion.exe never landed. (c) `93752cb` — `docs/INSTALL.md`: plain-language fresh-machine guide for non-technical users. SETUP.md stays as the technical reference; INSTALL.md is the quick-start.
- **2026-05-22** — Moonshot K2.6 work: temperature clamp for K2.* (`752030f`), Moonshot pair `--provider kimi` (`c74a358`), per-phase latency telemetry (`fa34cd5`), pre-gen-skip widening to 400 chars on Kimi (`64ab858`), `kimi_max_concurrent` semaphore primitive (default disabled, `54ca71b`/`43c551e`), stress harness fixes (`b5e6e84`/`058cc5e`). Earlier in same day: gaps #1, #3, #4, #5, #6 closed (`8cce939`, `be7fe82`) — per-role model selection, self-eval breaker, `get_user_recent_activity` fail-closed, Electron Python bundling, peer token streaming, write_file widened to machine-wide.
- **2026-05-21** — cross-interface session sync, location services, cross-user retrieval, fresh-session-per-launch, web push timer landed (see those sections).
- **2026-04-25** — read tools opted in to machine-wide paths (invariant #7).

## Working style

- Push back when a request contradicts an invariant.
- Ask when unclear — don't guess toward the wrong target.
- Flag output that looks profound but may be prompt artifact.
- **Per-gap commit splits.** When closing multiple distinct gaps/issues in one session, ship each as its own commit. Plan boundaries upfront so changes stay separable; don't batch at the end. (The matching `git add -p` is interactive and doesn't work well through tooling, so the practical pattern is: finish gap N, commit, then start gap N+1.) Prompt changes always get a commit note explaining why.
- Verification harnesses ship in the same commit as the feature they verify, not later.
- Never commit API keys. `.env` is gitignored. `config.save()` strips key fields.
