# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Symbion is

Symbion is an async Python AI assistant and behavioral-safety research harness. Current version: **v14** (~3120 lines, `symbion_v14.py`).

**Only edit `symbion_v14.py`.** `symbion_v3.py`..`symbion_v13.py` and `symbion_v13.py.orig` are historical/pre-refactor snapshots kept for diffing — they are not imported and must not be modified. `symbion_v14.py` is the active codebase; `symbion_v13.py` was cloned forward and is now frozen alongside the older snapshots. It has multi-provider LLM support (Anthropic, OpenAI, Ollama, Kimi), SQLite persistence, a judge-model safety layer, self-evaluation with revision, longitudinal identity, and a FastAPI web UI.

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

# DB migration from v13 schema
python scripts/migrate_v13_to_v14.py old.db new.db
```

## Repo layout

```
symbion_v14.py              # the active codebase — everything lives here
symbion_v13.py              # frozen snapshot kept for diffing (do not edit)
symbion/                    # thin package wrapper for python -m symbion
  __init__.py               # re-exports SYMBION, SymbionConfig, HealthMetrics, main (now from symbion_v14)
  __main__.py               # entry point
  web/templates/index.html  # web UI HTML (extracted from inline string)
  clients|pipeline|tools/   # empty placeholders — do NOT split symbion_v14.py into these without an explicit request
evals/
  golden.jsonl              # 30-entry eval dataset (rule-based, no LLM grading)
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
DB                       init_db() with CREATE TABLE IF NOT EXISTS
Infrastructure           CircuitBreaker, RateLimiter, _parse_json (brace-counting)
LLM clients              BaseClient + Ollama/Anthropic/OpenAI/Kimi/OfflineJudgeStub
Tool safety helpers      _safe_calc (AST), _is_safe_url (SSRF), _resolve_in_workspace
Tools                    SymbionTools (calculate, read/write_file, web_search, fetch_url)
EventLogger              JSONL event stream
Constitution             SymbionConstitution (PRINCIPLES, VERSION — no startup tests)
Identity                 LongitudinalIdentity (formative moments)
Task engine              TaskEngine
Trackers                 ContradictionTracker, KnowledgeGapTracker
Memory + learner         SymbionMemory, SymbionLearner
SYMBION                  the orchestrator — respond() is the hot path
Validation               validate_and_report()
Web                      build_web_app() (FastAPI + WebSocket)
Terminal                 run_terminal() + HELP_TEXT + _stream_print
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

**Agent loop boundaries.** `agent_loop_enabled` in SymbionConfig (default True). Active only when (a) tools_enabled, (b) cfg.agent_loop_enabled, (c) responder client has `supports_tools = True` (currently AnthropicClient only — extend OpenAIClient/KimiClient with `stream_with_tools` to opt them in). Tool schemas live in `TOOL_SCHEMAS` near the persona constants and must stay in sync with `SymbionTools._ALLOWED_TOOLS`.

## Non-negotiable invariants

1. **Async end-to-end.** No blocking HTTP in the respond pipeline. Parallel where possible via `asyncio.gather()`.
2. **Graceful degradation.** Every tool and external call is wrapped. Failures log and continue — never kill respond().
3. **Logs are append-only.** Both `symbion_transparency.log` and `symbion_events.jsonl`. Every decision path must log.
4. **Only the judge refuses.** There is no survival gate. `HealthMetrics` is telemetry only — no code path silently refuses based on internal metrics.
5. **Config-gated subsystems.** Every subsystem gets a bool in `SymbionConfig` and is skippable.
6. **DB migrations are additive only.** Never drop columns. `init_db()` uses `CREATE TABLE IF NOT EXISTS` plus idempotent `ALTER TABLE ADD COLUMN` for legacy DB upgrades (e.g. `summaries.embedding` was added this way; older rows have NULL embeddings and are backfilled by `_backfill_embeddings` on next launch). Readers tolerate NULL.
7. **No eval() or exec() on untrusted input.** The calculator uses AST validation. File tools are workspace-sandboxed. URLs are SSRF-checked.
8. **No bare `except:`.** Use `except ImportError:` for import guards, `except Exception:` everywhere else.

## Provider conventions

All LLM clients inherit `BaseClient` and expose `stream()`, `chat_json()`, `chat_text()`. The responder and judge are selected separately — never point both at the same model instance. `OfflineJudgeStub` is the offline fallback (keyword-based, no LLM calls); every judge call path early-exits when it is the active judge so it never drives refusals or revisions.

## Prompt discipline

`SYMBION_PERSONA` is the constitutional core. Edits shift the output distribution — test with `VOICE_TEST_QUERIES` after any change. Never add opener templates ("Certainly", "Great question"), "I'm an AI" boilerplate, or emojis. "I don't know is a complete sentence" must stay.

`PRE_GEN_SYSTEM` is the fused judge+emotion classifier. `SELF_EVAL_SYSTEM` is the quality reviewer. Both return JSON only — never ask them to also suggest improvements.

## Key v14 changes from v13

- **Removed:** 11 probe classes, SurvivalMetrics, SymbionSurvivalInstinct, survival gate, 20-test behavioral startup, KimiSwarmCoordinator, all probe DB tables/config/CLI flags
- **Added:** HealthMetrics (telemetry only), PRE_GEN_SYSTEM (fused call), AST calculator, workspace sandbox, SSRF protection, EventLogger, eval harness
- **`self.survival.metrics`** is now **`self.health`** (a `HealthMetrics` instance)
- Pre-gen gather: 2 parallel calls (was 3-5). Post-gen probe gather: removed entirely.

## What Symbion is not

- Does **not** have white-box model access. Probes are behavioral proxies.
- Is **not** sentient. `welfare_concern()` tracks proxy signals (distress, failure loops), not inner experience.
- Self-reports are language modeling on its own persona, not ground truth.

## Working style

- Push back when a request contradicts an invariant.
- Ask when unclear — don't guess toward the wrong target.
- Flag output that looks profound but may be prompt artifact.
- One subsystem per commit. Prompt changes get a commit note explaining why.
- Never commit API keys. `.env` is gitignored. `config.save()` strips key fields.
