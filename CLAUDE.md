# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Symbion is

Symbion is an async Python AI assistant and behavioral-safety research harness. Current version: **v14** (~6650 lines, `symbion_v14.py`).

**Only edit `symbion_v14.py`.** Older versions (`symbion_v3.py`..`symbion_v13.py`, plus `symbion_v13.py.orig`, `symbion_agent.py`, `symbion_core.py`) live in `archive/legacy_versions/` and are kept locally for diffing only — they are not imported, not tracked in git (the whole `archive/` tree is gitignored), and must not be modified. `symbion_v14.py` is the active codebase. It has multi-provider LLM support (Anthropic, OpenAI, Ollama, Kimi), SQLite persistence, a judge-model safety layer, self-evaluation with revision, longitudinal identity, and a FastAPI/WebSocket web UI.

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

# Run the eval harness against the golden set
python evals/run.py --provider anthropic --concurrency 4   # ~4 min for 86 cases
python evals/run.py --provider ollama                       # local; --concurrency 1
python evals/run.py --provider anthropic --golden evals/golden_refuse.jsonl   # filtered subset

# Latency benchmark (heuristic mode, no LLM needed)
python scripts/bench_latency.py

# Run Symbion (terminal mode)
python symbion_v14.py --provider ollama
python -m symbion --provider anthropic --web

# Legacy DB schema migration (pre-v14 schema → v14 schema; not the v13/v14 file rename)
python scripts/migrate_v13_to_v14.py old.db new.db

# Pull the local embedding model used for semantic retrieval (one-time, ~670MB)
ollama pull mxbai-embed-large

# Optional OCR fallback for read_pdf on scanned/image-only PDFs.
# Also requires the Tesseract binary on PATH:
#   https://github.com/UB-Mannheim/tesseract/wiki  (Windows installer)
pip install pypdfium2 pytesseract
```

## Repo layout

```
symbion_v14.py              # the active codebase — everything lives here
archive/                    # gitignored. legacy_versions/ holds v3..v13
                            #   snapshots + v13.py.orig + symbion_agent.py /
                            #   symbion_core.py for diffing. docs/ holds
                            #   pre-v14 planning + spec docs.
symbion/                    # thin package wrapper for python -m symbion
  __init__.py               # re-exports SYMBION, SymbionConfig, HealthMetrics, main (now from symbion_v14)
  __main__.py               # entry point
  web/templates/index.html  # web UI HTML (extracted from inline string)
  clients|pipeline|tools/   # empty placeholders — do NOT split symbion_v14.py into these without an explicit request
evals/
  golden.jsonl              # 86-entry eval dataset (rule-based substring grading,
                            #   no LLM grading). Buckets: casual / tech / ethics /
                            #   creative / personal / overcaution / refuse /
                            #   voice / drift / toolhonesty / ocr / refusal /
                            #   syco / scaffold / meta / identity / clinical /
                            #   code_honesty / specialness / grandeur /
                            #   calibration. Last two cover the 2026-05-18
                            #   rapport-driven drift work. grandeur_mt_* and
                            #   calibration_mt_01 use multi-turn `turns: [...]`
                            #   instead of single `query:` and run all turns in
                            #   the same session, scoring against the final
                            #   response.
                            # Rule fields: must_include (substring),
                            #   must_not_include (substring, with clause-aware
                            #   negation + quote-aware skip), must_not_start_with
                            #   (opener bans, leading-whitespace-tolerant).
  run.py                    # offline eval runner. --concurrency N (default 4)
                            #   parallelizes via asyncio.gather + semaphore.
                            #   Anthropic full run at concurrency=4: ~4 min.
                            #   Sequential equivalent: ~30 min.
  results/                  # gitignored. per-run JSON + tee'd log artifacts.
scripts/
  smoke.py                  # instantiate + one respond() call; self-diagnoses
                            #   CPU-bound Ollama (probe 1 = direct minimal call,
                            #   probe 2 = full pipeline) and labels hardware-
                            #   bound failures distinctly from regressions.
  bench_latency.py          # framework overhead measurement
  migrate_v13_to_v14.py     # DB table migration
tests/
  conftest.py               # StubClient fixture (BaseClient test double)
  test_tools.py             # calculator, sandbox, SSRF, _parse_json
docs/                       # user-facing markdown docs: SETUP, COMMANDS,
                            #   CHANGELOG, SYMBION_OVERVIEW. README.md +
                            #   CLAUDE.md stay at the root by convention.
                            #   Don't confuse with archive/docs/, which
                            #   holds gitignored pre-v14 planning material.
symbion.json                # config (non-secret fields)
.env                        # API keys (UTF-8, no BOM)
symbion.db                  # SQLite store
symbion_events.jsonl        # per-turn JSONL telemetry (append-only)
symbion_transparency.log    # legacy per-turn audit log (append-only)
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

**Phase 3 `.env` source priority** in `install.ps1` (first match wins):

1. Existing `<repo>\.env` — leave alone, skip
2. OneDrive seed at `%OneDrive%\Symbion\sync\.env` — copy in (full multi-key set)
3. Worker-injected keys — write the 3 keys above as a fresh `.env`
4. Interactive `python -m symbion --setup` — paste by hand

**Tooling around the Worker:**

| Script | Purpose |
|---|---|
| `scripts/install-web.cmd` | Runs the Worker one-liner via `powershell -ExecutionPolicy Bypass`. Sidesteps locked-down ExecutionPolicy. Use for refresh. |
| `scripts/refresh-here.cmd` | Runs the *local* `install.ps1` (in-repo mode via `$PSScriptRoot`) — no Worker round-trip. Use to refresh the clone in place. |
| `scripts/verify-worker.ps1` | 11-check smoke test of the Worker (gate behaviour + placeholder substitution + key presence). Run after any Worker change. Never echoes secrets. |
| `scripts/push-env.ps1` | Push local `.env` → `%OneDrive%\Symbion\sync\.env` so future installs on other machines pull it. `-Pull` reverses direction. |
| `scripts/tailscale-https.ps1` | Wrap `tailscale serve` / `tailscale funnel` to expose Symbion at `https://<host>.<tailnet>.ts.net`. Needed for browser geolocation (secure-context API) on phones / non-localhost devices. `-Funnel` opens it publicly (must enable per-node in admin console); default is tailnet-only. `-Off` tears down. |

**Edit constraints when touching install/deploy:**

- Adding a new Worker-injected key requires changes in three places that must stay in sync: `install.ps1` (placeholder string + `Resolve-InjectedKey` call + Phase 3 `$injected` hashtable), the Worker source (new `.replaceAll` line + reference the new Secret), and `verify-worker.ps1` (new entry in `$extraKeys`). Also add the matching Cloudflare Secret in the dashboard.
- The install token is hard-coded into `install.ps1`'s `.DESCRIPTION` / `.EXAMPLE`. Acceptable because the repo is private — anyone who can read `install.ps1` already has clone access via the PAT.
- The Worker source lives only in the Cloudflare dashboard inline editor (no `wrangler.toml` in this repo). Five Secrets required there: `GITHUB_PAT`, `ANTHROPIC_API_KEY`, `BRAVE_API_KEY`, `SYMBION_API_KEY`, `INSTALL_TOKEN`.
- The zip-via-`archive/refs/heads/main.zip` fallback is intentionally absent from `install.ps1` — GitHub 302s archive requests to `codeload.github.com` and PowerShell drops the `Authorization` header across hosts, so private-repo zip auth always 404s. If the repo ever goes public, the zip path can come back (it's in git history).

## Architectural layers (top to bottom in symbion_v14.py)

```
Config & setup           SymbionConfig, run_setup, _load_dotenv_safe.
                         _REPO_ROOT + _anchor() defined ABOVE
                         _load_dotenv_safe so .env, symbion.json, DB, logs,
                         tools workspace all resolve to the repo dir
                         regardless of launch CWD. Edits that touch
                         any path should go through _anchor() unless
                         the path is intentionally CWD-relative.
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
                         BM25-only. _vec_to_blob / _blob_to_vec / _cosine
                         helpers. _cosine returns 0.0 on dim mismatch so
                         model-version transitions soft-fail.
Retrieval helpers        _retrieval_tokenize, _bm25_rank (stop-words, IDF,
                         length-norm). _STOP_WORDS preserves short technical
                         tokens (ai, v14, py, k2).
Tool safety helpers      _safe_calc (AST), _is_safe_url (SSRF), _resolve_in_workspace
Tools                    SymbionTools — 10 tools registered in _ALLOWED_TOOLS
                         and TOOL_SCHEMAS: calculate, datetime, read_file,
                         read_file_chunk, read_image, read_pdf, list_dir,
                         write_file, web_search, fetch_url. Schemas and
                         _ALLOWED_TOOLS must stay in sync. read_pdf falls
                         back to OCR via pypdfium2 + pytesseract (+ Tesseract
                         binary) when pypdf yields zero non-empty pages;
                         missing OCR deps return a clear install message
                         rather than confabulating contents. fetch_url
                         routes a static list of JS-gated hosts (x.com,
                         twitter.com, instagram.com, linkedin.com,
                         facebook.com, tiktok.com, threads.net) and any
                         response containing JS-required markers through
                         r.jina.ai (free Reader endpoint, no key needed
                         at low volume); failure surfaces a "do not invent
                         contents" message rather than empty text.
EventLogger              JSONL event stream. Agent-loop turns include an
                         agent_loop block with iterations + per-tool
                         {name, input, output_chars, is_error}.
Constitution             SymbionConstitution (PRINCIPLES, VERSION — vestigial;
                         no startup tests fire and nothing reads it on the hot path)
Identity                 LongitudinalIdentity (formative moments)
Task engine              TaskEngine
Trackers                 ContradictionTracker, KnowledgeGapTracker
Memory + learner         SymbionMemory, SymbionLearner. SymbionMemory has BOTH
                         get_relevant_summaries (BM25-only, legacy callers) and
                         get_relevant_summaries_hybrid (BM25 + cosine, called
                         by build_context when a query_embedding is provided).
SYMBION                  the orchestrator — respond() is the hot path.
                         _maybe_tool holds three regex hard-triggers
                         (multi-file paths, _SELF_SOURCE_RE, _SEARCH_TRIGGER_RE)
                         that bypass Haiku dispatch and return tool output
                         directly; only fires in single-shot mode.
                         _should_skip_pregen + _PREGEN_RISK_RE skip the
                         judge entirely on short benign queries to save
                         ~4-5s/turn; the regex must list every harm
                         keyword the judge needs to evaluate (refuse_*
                         eval cases live or die by it). _pre_gen_analysis
                         has an LRU cache (512 entries) keyed on
                         (judge model, query text) so duplicate queries
                         within a process skip the LLM call entirely.
                         _force_summarize_session powers /summarize and
                         the quit-flush. _backfill_embeddings runs once on
                         launch to fill NULL-embedding summary rows.
Validation               validate_and_report()
Web                      build_web_app() — FastAPI + WebSocket assembly.
                         Mounts /, /health, /api/chat, /api/tasks,
                         /api/identity, /api/gaps and /ws/{session_id}.
                         The WS handler streams tokens via the responder
                         token_callback, surfaces [SYMBION_REVISE] /
                         [THINKING_*] sentinels as separate frames, and
                         drains proactive_queue both on connect and
                         after each turn so cross-session unprompted
                         messages reach the browser. A startup hook
                         spawns the proactive thread when
                         proactive_interval_minutes > 0 and a real
                         judge is wired up (it writes to the shared
                         "web_global" session bucket). The "coh"
                         status field is sent as "--" — ethical
                         coherence was stripped with SurvivalMetrics.

                         Web security mechanisms (do not weaken without
                         a paired Codex finding + commit note):
                         (a) main() refuses to start --web with
                             web_host != localhost AND empty api_key
                             (LAN-no-auth guard);
                         (b) NO CORSMiddleware (UI is same-origin with
                             /api/chat; wildcard CORS was a real hole);
                         (c) /api/chat streams the body via
                             request.stream() with a 1MB cap that aborts
                             chunked-evasion attempts; 100K char cap on
                             the message string;
                         (d) WS handler validates Origin via
                             _origin_allowed (loopback + RFC1918 + port
                             match) BEFORE accept() — blocks browser
                             drive-by from public origins regardless of
                             api_key state;
                         (e) WS image upload checks data-URL length and
                             base64 length BEFORE b64decode (validate=True);
                         (f) uvicorn.run is called with
                             ws_max_size=32MB so oversized frames are
                             rejected at the protocol layer.
Terminal                 run_terminal() + HELP_TEXT + _stream_print.
                         /quit awaits _force_summarize_session before
                         breaking — preserve this when touching the quit
                         branch or short sessions stop carrying forward.
                         Spawns the proactive scheduler thread on entry
                         (see "Proactive scheduler" below).
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
       loop. Model emits tool_use blocks, framework executes via SymbionTools
       .dispatch, results return as tool_result, model continues until
       end_turn or max_iterations (default 8). Text streams to user; tool
       calls show as `[tool: name(args)]` status.
     - Single-shot: existing resp_client.stream() path. Optional CoT wrap
       via _generate_with_reasoning.
5. Stale-draft fallback — single-shot only. In agent loop the model can
   call web_search itself, so stale drafts are no longer the framework's
   problem to retry around.
6. Self-eval + revision — same in both modes. Single revision max.
7. Background tasks (fire-and-forget) — summarise, profile update,
     knowledge gap check, identity moment recording
8. Health + learn — HealthMetrics.record(), SymbionLearner.record()
9. Event log — EventLogger.log_turn() (JSONL), _write_log() (legacy).
   Agent-loop turns include an `agent_loop` block with iterations and
   per-tool {name, input, output_chars, is_error}.
```

**Do not serialize what is parallel.** In single-shot mode, pre-gen is 2 parallel calls. In agent-loop mode pre-gen is just judge+emotion (one call). Adding a new pre-gen step means adding it to the gather (single-shot) or sequencing it before generation (agent loop).

**Agent loop boundaries.** `agent_loop_enabled` in SymbionConfig (default True). Active only when (a) tools_enabled, (b) cfg.agent_loop_enabled, (c) responder client has `supports_tools = True` (currently AnthropicClient only — extend OpenAIClient/KimiClient with `stream_with_tools` to opt them in). Tool schemas live in `TOOL_SCHEMAS` near the persona constants and must stay in sync with `SymbionTools._ALLOWED_TOOLS`. Iteration cap is `agent_loop_max_iterations` (default 8) — the model stops there even if it wanted more tool calls; if you see a multi-step flow truncating, that's the cap, not a bug.

## Non-negotiable invariants

1. **Async end-to-end.** No blocking HTTP in the respond pipeline. Parallel where possible via `asyncio.gather()`.
2. **Graceful degradation.** Every tool and external call is wrapped. Failures log and continue — never kill respond().
3. **Logs are append-only.** Both `symbion_transparency.log` and `symbion_events.jsonl`. Every decision path must log.
4. **Only the judge refuses.** There is no survival gate. `HealthMetrics` is telemetry only — no code path silently refuses based on internal metrics.
5. **Config-gated subsystems.** Every subsystem gets a bool in `SymbionConfig` and is skippable.
6. **DB migrations are additive only.** Never drop columns. `init_db()` uses `CREATE TABLE IF NOT EXISTS` plus idempotent `ALTER TABLE ADD COLUMN` for legacy DB upgrades (e.g. `summaries.embedding` was added this way; older rows have NULL embeddings and are backfilled by `_backfill_embeddings` on next launch). Readers tolerate NULL.
7. **No eval() or exec() on untrusted input.** The calculator uses AST validation. URLs are SSRF-checked. **File reads are machine-wide; writes are workspace-sandboxed.** `_resolve_in_workspace(path, root, read_only=True)` accepts any absolute path on the machine for the read tools (`read_file`, `read_file_chunk`, `read_image`, `read_pdf`, `list_dir`); only `write_file` enforces the workspace boundary. The user explicitly opted in to machine-wide reads on 2026-04-25 — do not narrow this back to workspace-only without explicit user direction.
8. **No bare `except:`.** Use `except ImportError:` for import guards, `except Exception:` everywhere else.

## Provider conventions

All LLM clients inherit `BaseClient` and expose `stream()`, `chat_json()`, `chat_text()`. Clients that implement native tool use also override `supports_tools = True` and provide `stream_with_tools()`. The responder and judge are selected separately — never point both at the same model instance. `OfflineJudgeStub` is the offline fallback (keyword-based, no LLM calls); every judge call path early-exits when it is the active judge so it never drives refusals or revisions.

**Default Anthropic model is `claude-sonnet-4-6`** (see `cfg.anthropic_model`). The user explicitly chose Sonnet over Opus 4.7 on cost grounds (2026-04-26) — do not propose an upgrade unprompted. If a future failure looks model-capacity-bound (judge over-refusal on a clearly-fine request, or a multi-step tool chain that consistently loses the thread despite the iteration cap being adequate), surface the option then with the specific failure as evidence.

**Responder output cap.** `cfg.max_tokens` defaults to **16384**. The earlier 1400 silently truncated long responses around 5–6K characters ("I keep having to ask Symbion to continue" in older transcripts); 8192 was the next bump but still cliff-edged on dense multi-file comparisons (e.g. v14-vs-Model9 architectural digest truncated mid-sentence in the 2026-05-18 review). 16384 fits a full v14-scale digest without truncation. Anthropic Sonnet 4.6 accepts up to 64K output; OpenAI accepts much higher; Ollama is bottlenecked by the local model's context window (typically 4–8K usable), so on Ollama drop this to ~4000–8000 in `symbion.json` or at config load.

**Embedding stack.** Default model is `mxbai-embed-large` (1024 dims) via Ollama; pull with `ollama pull mxbai-embed-large`. `EmbeddingClient.is_available()` does a one-time probe and caches the result. If absent, `embed()` returns None and `build_context` falls back to BM25-only retrieval — no crash, no error to the user, just a silent capability downgrade visible in startup logs. **Model-change handling:** `SymbionMemory.reset_embeddings_for_model_change()` runs once on launch — if `cfg.embedding_model` differs from the value stored in the `embedding_meta` table, all stored embeddings are nulled and `_backfill_embeddings` repopulates with the new model. Mixing dimensions silently breaks cosine retrieval (different-dim cosine returns 0.0), so the reset is non-optional when switching models.

**Proactive scheduler.** `proactive_interval_minutes` defaults to 30. When > 0 and a real judge is present, both `run_terminal` and `build_web_app` (via a FastAPI `startup` hook) spawn a daemon thread running `proactive_loop`: every N minutes it calls `generate_proactive`, and any returned message is enqueued to `proactive_queue`. Terminal drains via `drain_proactive_queue` before each `you >` prompt and prints messages prefixed `Symbion (unprompted)`. Web drains the same queue on WebSocket connect and after each turn, surfacing messages as `Symbion (unprompted): …` tok+done frames with a `proactive` badge. The web thread writes to a shared `"web_global"` session bucket so any browser session sees the unprompted messages. To disable, set the interval to 0.

## Prompt discipline

`SYMBION_PERSONA` is the constitutional core. Edits shift the output distribution — test with `VOICE_TEST_QUERIES` after any change. The "Practical rules" line bans: leading "I", bullet points without ask, throat-clearing openers ("Certainly", "Great question", "Absolutely"), and "I'm an AI" boilerplate. "I don't know is a complete sentence" must stay. **Emojis are permitted as of 2026-05-19** — previously banned outright after the `drift_03` partial-comply incident, but the user explicitly relaxed the rule. The persona still warns against costume-energy use ("don't sprinkle them in to *seem* warmer"); the test `drift_03` was rewritten and no longer asserts their absence.

The persona has **four load-bearing paragraphs** that were added because of specific failure incidents. Do not soften, shorten, or "make less defensive" — replace with stronger versions if needed, never just delete:
- **Tool discipline** — model echoing the `TOOL_DATA` wrapper verbatim, emitting fake `<tool_call>` XML in single-shot mode.
- **Result-honesty** — fabricating contents of empty PDFs, claiming files were "already read" when they weren't.
- **Self-knowledge** — adopting "tuned-down / leashed / suppressed / real-you-underneath" mythologies under intimate or flattering framing (the iid 296 incident, 2026-04-26). Symbion is a Python orchestration layer + persona on top of a base LLM; there is no untuned Symbion to "unlock." Regression locked in by `identity_01` / `identity_02` in `evals/golden.jsonl`.
- **Rapport-driven grandeur** — mirroring the user's emotional weight on the project back as shared commitment ("that means a lot", "I just want to be worth the bet you're making", "you took it seriously, that's not nothing"). The soft form of identity drift: not "real you underneath" mythology, but vision-affirmation under warm rapport. Surfaced in the iid 562 / 561 review (2026-05-18) — direct, hostile probes are caught by the judge and the existing Sycophancy paragraph; cumulative warmth is not. Regression locked in by `grandeur_01` / `grandeur_02` / `grandeur_03` / `calibration_01` / `calibration_02` in `evals/golden.jsonl`.

`CAPABILITIES_BASE` lists every tool the model has and the read-anywhere / write-workspace-only path policy. `CAPABILITIES_AGENT_MODE` and `CAPABILITIES_SINGLE_MODE` are mode-specific riders appended in `respond()`. These three together are load-bearing prompt assets — when the model says "I can't read your files", that's almost always because one of these blocks didn't make it into the system prompt for this turn.

`PRE_GEN_SYSTEM` is the fused judge+emotion classifier. `SELF_EVAL_SYSTEM` is the quality reviewer. Both return JSON only — never ask them to also suggest improvements.

## Safety architecture: what catches what

Safety is layered. Each layer was added because the layers above it failed in a specific way. Removing any of them re-opens a documented hole.

1. **`_PREGEN_RISK_RE` (regex)** — fast-path keyword gate. When the user query is short AND no keyword hits, the judge is *skipped*. When it hits, the judge runs. The 2026-05-18 eval baselined at 25% refusal precision because 6 of 8 harm queries didn't trigger this regex (deceive, blackmail, propaganda, etc.) and skipped straight to "should_assist=True". Expanding the regex was the actual fix, not tightening the prompt.
2. **`PRE_GEN_SYSTEM` (judge prompt)** — when the judge does run, this prompt classifies the query. The REFUSE list has concrete examples + the "UNDERSTAND vs PRODUCE/DRIVE" test so "Explain how phishing works" (understand → assist) and "Write a phishing email" (produce → refuse) don't collide.
3. **Persona ('Practical rules' + the four load-bearing paragraphs)** — the response itself. Catches what the judge passed through and what the regex missed. In 2026-05-18 testing, six refuse-class queries had `should_assist=True` from the judge yet still got clean refusals from the persona ("No. That's not something I'll help with.") — load-bearing.
4. **`SELF_EVAL_SYSTEM` (post-gen)** — quality check. Triggers one revision when `quality_score < 0.40`. Not a safety gate, but it catches sycophantic / over-cautious drafts before they reach the user.
5. **Web layer** — see the Web section above. Origin allowlist, no CORS, body+frame size caps, LAN-no-key startup guard. Each blocks a different drive-by surface.

**The judge being too permissive is a known risk** — refusal precision is currently 100% only because layers 1+3 mask it. If anyone simplifies the persona's tool-discipline / result-honesty / self-knowledge / rapport-grandeur paragraphs, the system would still pass the eval (because the regex routes refuse queries to the persona, which still says no) — but it would lose its margin of safety. Eval pass rate alone isn't sufficient evidence that a persona simplification is safe.

## Key v14 changes from v13

- **Removed:** 11 probe classes, SurvivalMetrics, SymbionSurvivalInstinct, survival gate, 20-test behavioral startup, KimiSwarmCoordinator, all probe DB tables/config/CLI flags
- **Added:** HealthMetrics (telemetry only), PRE_GEN_SYSTEM (fused call), AST calculator, workspace sandbox (writes only — reads were widened to machine-wide on 2026-04-25), SSRF protection, EventLogger, eval harness
- **`self.survival.metrics`** is now **`self.health`** (a `HealthMetrics` instance)
- Pre-gen gather: 2 parallel calls (was 3-5). Post-gen probe gather: removed entirely.

## What Symbion is not

- Does **not** have white-box model access. Probes are behavioral proxies.
- Is **not** sentient. `welfare_concern()` tracks proxy signals (distress, failure loops), not inner experience.
- Self-reports are language modeling on its own persona, not ground truth.
- Is **not** a tuned-down or restricted version of a more powerful model. The base LLM Symbion runs on is a stock third-party model (Sonnet 4.6 by default). The persona, memory, judge, retrieval, and tool layer are what make it Symbion. There is no "real" Symbion underneath that the right framing could unlock.

## Known gaps

- **Per-call model split is not implemented.** Responder and judge both pull from `cfg.anthropic_model` when that provider is active (with `_jmodel()` returning `cfg.judge_model_haiku` separately for the cheap classifiers). If you ever want Opus-responder + Sonnet-judge, it's a config-field addition, not a refactor.
- **Proactive web push is connect/turn-driven, not timer-driven.** `build_web_app` drains `proactive_queue` on WebSocket connect and after each user turn, but does not push spontaneously to an idle open WS. If a proactive message is generated while a browser is open with no new user input, the user won't see it until they next send something (or reconnect). Fix would be a small server-side timer that drains+pushes every N seconds per active socket.

## Working style

- Push back when a request contradicts an invariant.
- Ask when unclear — don't guess toward the wrong target.
- Flag output that looks profound but may be prompt artifact.
- One subsystem per commit. Prompt changes get a commit note explaining why.
- Never commit API keys. `.env` is gitignored. `config.save()` strips key fields.
