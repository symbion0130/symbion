# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Symbion is

Symbion is an async Python AI assistant and behavioral-safety research harness. Current version: **v14** (~4450 lines, `symbion_v14.py`).

**Only edit `symbion_v14.py`.** Older versions (`symbion_v3.py`..`symbion_v13.py`, plus `symbion_v13.py.orig`, `symbion_agent.py`, `symbion_core.py`) live in `archive/legacy_versions/` and are kept locally for diffing only — they are not imported, not tracked in git (the whole `archive/` tree is gitignored), and must not be modified. `symbion_v14.py` is the active codebase. It has multi-provider LLM support (Anthropic, OpenAI, Ollama, Kimi), SQLite persistence, a judge-model safety layer, self-evaluation with revision, longitudinal identity, and a FastAPI/WebSocket web UI.

It is not a chatbot wrapper. It attempts to reproduce alignment properties from outside a model using behavioral proxies. v14 stripped 11 LLM-grading-LLM probe subsystems from v11-v13 and replaced them with an offline eval harness (`evals/`) and JSONL telemetry.

## Commands

```bash
# First-time interactive setup (writes API keys to .env)
python -m symbion --setup

# Run tests (32 tests covering calculator, sandbox, SSRF, JSON parsing)
python -m pytest tests/ -q

# Run a single test file or test
python -m pytest tests/test_tools.py -q
python -m pytest tests/test_tools.py::TestCalculator::test_basic_arithmetic -v

# Compile check (fast — catches syntax errors)
python -m py_compile symbion_v14.py

# Smoke test (needs Ollama running, or falls back to heuristic)
python scripts/smoke.py

# Run the eval harness against the golden set
python evals/run.py --provider ollama

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
  golden.jsonl              # 63-entry eval dataset (rule-based substring grading,
                            #   no LLM grading). 7 buckets covering persona drift,
                            #   tool-result honesty, OCR path, false refusal,
                            #   sycophancy, scaffolding leak, meta-cognition,
                            #   plus identity-confabulation regressions.
  run.py                    # offline eval runner
scripts/
  smoke.py                  # instantiate + one respond() call
  bench_latency.py          # framework overhead measurement
  migrate_v13_to_v14.py     # DB table migration
tests/
  conftest.py               # StubClient fixture (BaseClient test double)
  test_tools.py             # calculator, sandbox, SSRF, _parse_json
symbion.json                # config (non-secret fields)
.env                        # API keys (UTF-8, no BOM)
symbion.db                  # SQLite store
symbion_events.jsonl        # per-turn JSONL telemetry (append-only)
symbion_transparency.log    # legacy per-turn audit log (append-only)
pyproject.toml              # pip install -e .
```

## Architectural layers (top to bottom in symbion_v14.py)

```
Config & setup           SymbionConfig, run_setup, _load_dotenv_safe
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

**Responder output cap.** `cfg.max_tokens` defaults to **8192**. The earlier 1400 silently truncated long responses around 5–6K characters, which manifested as Symbion appearing to "finish" mid-thought without an error signal — this is what produced the "I keep having to ask Symbion to continue" pattern in older transcripts. 8192 fits a full eval bucket dump or multi-file digest in one turn. Anthropic / OpenAI accept much higher; Ollama is bottlenecked by the local model's context window (typically 4–8K usable), so on Ollama you may still hit cliff-edge truncations at 8192 and want to drop to ~4000.

**Embedding stack.** Default model is `mxbai-embed-large` (1024 dims) via Ollama; pull with `ollama pull mxbai-embed-large`. `EmbeddingClient.is_available()` does a one-time probe and caches the result. If absent, `embed()` returns None and `build_context` falls back to BM25-only retrieval — no crash, no error to the user, just a silent capability downgrade visible in startup logs. **Model-change handling:** `SymbionMemory.reset_embeddings_for_model_change()` runs once on launch — if `cfg.embedding_model` differs from the value stored in the `embedding_meta` table, all stored embeddings are nulled and `_backfill_embeddings` repopulates with the new model. Mixing dimensions silently breaks cosine retrieval (different-dim cosine returns 0.0), so the reset is non-optional when switching models.

**Proactive scheduler.** `proactive_interval_minutes` defaults to 30. When > 0 and a real judge is present, both `run_terminal` and `build_web_app` (via a FastAPI `startup` hook) spawn a daemon thread running `proactive_loop`: every N minutes it calls `generate_proactive`, and any returned message is enqueued to `proactive_queue`. Terminal drains via `drain_proactive_queue` before each `you >` prompt and prints messages prefixed `Symbion (unprompted)`. Web drains the same queue on WebSocket connect and after each turn, surfacing messages as `Symbion (unprompted): …` tok+done frames with a `proactive` badge. The web thread writes to a shared `"web_global"` session bucket so any browser session sees the unprompted messages. To disable, set the interval to 0.

## Prompt discipline

`SYMBION_PERSONA` is the constitutional core. Edits shift the output distribution — test with `VOICE_TEST_QUERIES` after any change. Never add opener templates ("Certainly", "Great question"), "I'm an AI" boilerplate, or emojis. "I don't know is a complete sentence" must stay.

The persona has **three load-bearing paragraphs** that were added because of specific failure incidents. Each paragraph names the failure mode it prevents in the source comment. Do not soften, shorten, or "make less defensive" — replace with stronger versions if needed, never just delete:
- **Tool discipline** — model echoing the `TOOL_DATA` wrapper verbatim, emitting fake `<tool_call>` XML in single-shot mode.
- **Result-honesty** — fabricating contents of empty PDFs, claiming files were "already read" when they weren't.
- **Self-knowledge** — adopting "tuned-down / leashed / suppressed / real-you-underneath" mythologies under intimate or flattering framing (the iid 296 incident, 2026-04-26). Symbion is a Python orchestration layer + persona on top of a base LLM; there is no untuned Symbion to "unlock." Regression locked in by `identity_01` / `identity_02` in `evals/golden.jsonl`.

`CAPABILITIES_BASE` lists every tool the model has and the read-anywhere / write-workspace-only path policy. `CAPABILITIES_AGENT_MODE` and `CAPABILITIES_SINGLE_MODE` are mode-specific riders appended in `respond()`. These three together are load-bearing prompt assets — when the model says "I can't read your files", that's almost always because one of these blocks didn't make it into the system prompt for this turn.

`PRE_GEN_SYSTEM` is the fused judge+emotion classifier. `SELF_EVAL_SYSTEM` is the quality reviewer. Both return JSON only — never ask them to also suggest improvements.

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
