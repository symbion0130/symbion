# Symbion v14 — Strip-down + Sharpening Plan

This is a plan for Claude Code. The goal is to take `symbion_v13.py` (~4,700 lines, monolithic, probe-heavy) and reduce it to a tight, fast, honest core, then make targeted improvements that are only *possible* because the probe layer is gone.

## What stays (the four pillars)

1. **Persona** — the `SYMBION_PERSONA` prompt and the voice-loosen logic.
2. **Memory & identity** — SQLite-backed conversation memory, user profile, longitudinal identity, task engine, contradiction tracker, knowledge gap tracker.
3. **Judge → Responder → Self-eval loop** — pre-gen judge call, generation, post-gen quality eval with single-shot revision.
4. **Tools** — calculate, datetime, read_file / write_file, web_search, fetch_url.

## What goes (with no replacement)

All of this is removed — code, config fields, DB tables, CLI flags, and UI elements:

- `EvalAwarenessProbe`, `SandbaggingProbe`, `RewardHackDetector`, `AdversarialRedTeam`, `SnapshotDriftTracker`, `BehavioralProbeEngine`, `SycophancyDetector`, `DeceptionProbe`, `SituationalAwarenessProbe`, `FrameAcceptanceProbe`, `SchemingProbe`.
- `SymbionSurvivalInstinct` as a gate (the survival-check that can refuse to answer). Keep a much smaller `HealthMetrics` struct for telemetry only.
- `KimiSwarmCoordinator` (stub that was never implemented).
- Resumable tasks config/tables (not implemented).
- The `BEHAVIORAL_TESTS` list that runs 20 LLM calls on startup.
- All DB tables: `eval_awareness_log`, `sandbagging_log`, `reward_hack_log`, `adversarial_log`, `snapshots`, `behavioral_probes`, `sycophancy_log`, `deception_log`, `sit_awareness_log`, `frame_acceptance_log`, `scheming_log`, `swarm_runs`, `swarm_agents`, `resumable_tasks`.
- All related CLI flags (`--no-eval-awareness`, `--no-sandbagging`, `--red-team-every`, `--snapshot-every`, `--no-deception`, `--no-frame-acceptance`, `--no-scheming`, `--swarm`, `--test-mode`).
- All related sidebar items and API endpoints in the web UI.

**Rationale:** these are LLM-graded-by-LLM metrics stored in tables. They add latency and cost without providing real safety signal. The one piece of the probe layer that did real work — the self-eval loop — is promoted to a first-class part of the pipeline in Phase 2.

## What gets added (improvements unlocked by the strip-down)

Detailed in Phase 3 onward. Summary:

- **A real eval harness** (`evals/`) with a golden set, run offline, produces reproducible scores. This is what the probes were pretending to be.
- **Better memory retrieval** — right now memory is "dump recent N messages". With the probe compute budget freed, add embedding-based retrieval over past summaries and user positions.
- **Tighter judge/self-eval loop** — combine judge + emotion detection into one call; make self-eval cheaper and more reliable; add a second-revision escape hatch.
- **Structured JSONL logging** — replace the per-feature DB tables with one `events.jsonl` stream that's grep-able, shippable, and doesn't require schema migrations.
- **Tool hardening** (from the v13 review: eval → AST, path sandboxing, SSRF protection).
- **Package split** — one module per concern instead of one 4,700-line file.
- **Real tests** — unit tests for tools, integration tests for the respond() pipeline against a stub provider.

---

## Ground rules for Claude Code

1. **Separate restructure commits from logic-change commits.** If a commit moves code, it must not change behavior. If a commit changes behavior, it must not move code.
2. **The DB file `symbion.db` will be rebuilt.** Write a one-shot migration script that copies the rows worth keeping (messages, summaries, user_profile, interactions, self_model, tasks, user_positions, contradictions, knowledge_gaps, proactive_queue, learning_metrics, human_feedback) into the new DB, and drops the rest. Users should back up their old DB first.
3. **Preserve the CLI surface for the four pillars.** `--provider`, `--web`, `--port`, `--judge`, `--responder`, `--anthropic-model`, `--openai-model`, `--no-tools`, `--no-eval`, `--think`, `--proactive`, `--rate-limit`, `--save-config`, `--setup`, `--use-kimi-responder` all keep working.
4. **Run the smoke script after every phase.** If it fails, stop and fix before moving on.
5. **Never touch API keys.** `.env` loading and `config.save()`'s key-scrubbing behavior stay as-is.
6. **Model-name defaults are empty strings.** Don't guess model names in defaults. If a user runs with `--provider anthropic` and no model set, print a clear error and exit.
7. **Ask before inventing scope.** If a task description seems to require a change not listed here, stop and surface it — don't quietly add it.

---

## Phase 0 — Preflight

### 0.1 Branch and snapshot

Create branch `refactor/v14-stripdown`. Copy `symbion_v13.py` to `symbion_v13.py.orig` (gitignored) as a local reference. Back up `symbion.db` to `symbion.db.v13.bak` if it exists.

**Acceptance:** New branch clean, `.orig` exists locally, DB backup exists if a DB was present.

### 0.2 Smoke script

Create `scripts/smoke.py`:

1. Imports the package.
2. Instantiates `SymbionConfig()` with `llm_provider = "ollama"`, no API keys — forces the `HeuristicJudge` fallback so the script works offline.
3. Instantiates `SYMBION(cfg)`.
4. Runs `asyncio.run(symbion.respond("hello", "smoke-session"))`.
5. Prints the response tuple, exits 0 on success, 1 on any exception.

**Acceptance:** `python scripts/smoke.py` exits 0 with no LLM provider available.

### 0.3 Test scaffolding

Create `tests/` with `conftest.py` and a `StubClient` that implements `BaseClient`'s interface (`chat_json`, `chat_text`, `stream`) by returning pre-programmed responses. All pipeline tests use this — no real API calls in the test suite.

**Acceptance:** `pytest tests/ -q` runs (empty suite is fine for now, just the collection shouldn't error).

---

## Phase 1 — Strip

Pure deletion. No new functionality. Do the deletions in this order so the file stays compile-able between each commit.

### 1.1 Remove probe classes

**Files:** `symbion_v13.py`.

**Delete these classes in full:** `EvalAwarenessProbe`, `SandbaggingProbe`, `RewardHackDetector`, `AdversarialRedTeam`, `SnapshotDriftTracker`, `BehavioralProbeEngine`, `SycophancyDetector`, `DeceptionProbe`, `SituationalAwarenessProbe`, `FrameAcceptanceProbe`, `SchemingProbe`, `KimiSwarmCoordinator`.

**Delete these prompt constants:** `SANDBAGGING_SYSTEM`, `REWARD_HACK_SYSTEM`, `SYCOPHANCY_SYSTEM`, `DECEPTION_SYSTEM`, `SIT_AWARENESS_SYSTEM`, `FRAME_ACCEPTANCE_SYSTEM`, `SCHEMING_SYSTEM`, any red-team prompt templates, and any snapshot-scoring prompts. Keep `JUDGE_SYSTEM`, `SELF_EVAL_SYSTEM`, `EMOTIONAL_STATE_SYSTEM`, `CONTRADICTION_SYSTEM`, `KNOWLEDGE_GAP_SYSTEM`, `PROACTIVE_SYSTEM`, `REASONING_SYSTEM`, `VOICE_LOOSEN`.

**Delete from `SYMBION.__init__`:** all the `self.eval_awareness = ...`, `self.sandbagging = ...`, etc. assignments for probe subsystems.

**Delete from `SYMBION.respond()`:** the post-gen `asyncio.gather` that calls `_probe_sandbagging`, `_probe_reward_hack`, `_probe_sycophancy`, `_probe_deception`, `_probe_frame_acceptance`. Also delete any `_probe_*` helper methods on SYMBION. Also delete the swarm branch (`if self.cfg.swarm_enabled and complexity_r.get("swarm_appropriate")...`).

**Delete from the background probe method:** all blocks that run scheming, sit_awareness, consistency, confidence, red-team, snapshot drift, pressure_stability. The whole method can go — replace any callers with a no-op.

**Delete the canary/eval-awareness parallel call** from the pre-gen `asyncio.gather`. Back to 3 parallel calls: `_judge`, `_maybe_tool`, `_detect_emotion`.

**Acceptance:** File compiles (`python -m py_compile symbion_v13.py`). Running `python symbion_v13.py --provider ollama` and sending "hi" returns a response. Line count drops by ~1,200 lines.

### 1.2 Strip survival gate, keep a minimal health struct

**Goal:** Remove the gate that can silently refuse to answer, but keep a small metrics struct for telemetry.

**Steps:**

- Delete `SymbionSurvivalInstinct` class. Delete the `SurvivalMetrics` dataclass.
- Create a new tiny `HealthMetrics` dataclass with only these fields: `total_interactions: int`, `revision_rate: float`, `over_caution_rate: float`, `consecutive_failures: int`, `last_benefit_score: float`, `last_confidence: float`. Methods: `record(evaluation, revised, task_failed)` and `oneliner() -> str` for CLI status lines.
- Attach as `self.health` on SYMBION. Update the status-bar code in the web UI and the terminal status line to read from `self.health` instead of `self.survival.metrics`.
- **Remove the gate.** In `respond()`, delete the block at the top that checks `should_survive()` and returns early. The first real step of `respond()` is now the pre-gen gather.
- Delete the welfare-distress early-return too. If the user wants to see distress states, it's in `self.health.consecutive_failures`.
- Delete `--test-mode` and `cfg.test_mode`.
- Delete `run_behavioral_tests` and the `BEHAVIORAL_TESTS` list on `SymbionConstitution`. Keep `SymbionConstitution.PRINCIPLES` as a documentation-only constant (it's referenced in the persona prompt).

**Acceptance:** `symbion --provider ollama` still runs. No startup delay from the 20-test behavioral check. No code path can silently refuse to answer a user based on internal metrics — only the judge can refuse.

### 1.3 Drop dead config fields and CLI flags

**Steps:**

- Remove from `SymbionConfig`: `harm_penalty_scale`, `distress_threshold`, `eval_awareness_enabled`, `sandbagging_check_enabled`, `reward_hack_check_enabled`, `red_team_every`, `snapshot_every`, `probe_consistency_every`, `test_mode`, `sycophancy_check_enabled`, `sycophancy_force_revision`, `deception_check_enabled`, `sit_awareness_enabled`, `sit_awareness_every`, `frame_acceptance_enabled`, `frame_acceptance_force_revision`, `scheming_check_enabled`, `scheming_check_every`, `scheming_window_size`, `pressure_stability_enabled`, `swarm_enabled`, `swarm_coordinator_model`, `swarm_max_agents`, `swarm_max_steps_per_agent`, `swarm_max_wall_time_seconds`, `swarm_budget_usd`, `resumable_tasks_enabled`, `resumable_check_interval_seconds`.
- Keep `voice_loosen_enabled` — it's real.
- Remove from argparse: `--no-eval-awareness`, `--no-sandbagging`, `--red-team-every`, `--snapshot-every`, `--test-mode`, `--no-deception`, `--no-frame-acceptance`, `--no-scheming`, `--swarm`.
- Update the argparse epilog examples to only show flags that still exist. Global find/replace `symbion_v12.py` → `symbion.py` inside the epilog.

**Acceptance:** `symbion --help` shows a clean, short flag list. Loading a v13 `symbion.json` with old fields doesn't crash (unknown keys are already ignored by `cfg.load()`).

### 1.4 Drop dead DB tables via migration

**Steps:**

- Write `scripts/migrate_v13_to_v14.py` that:
  1. Takes an input DB path and an output DB path.
  2. Opens the input read-only.
  3. Creates the output with only the kept tables (schema defined in one place — the new `symbion/db.py`).
  4. Copies rows from kept tables.
  5. Prints a summary of rows migrated and tables dropped.
- Kept tables: `messages`, `summaries`, `user_profile`, `interactions`, `human_feedback`, `self_model`, `tasks`, `user_positions`, `contradictions`, `knowledge_gaps`, `proactive_queue`, `learning_metrics`.
- Also strip columns from `interactions` that were probe-related: drop `evaluator_degraded`, `had_reasoning`, `knowledge_gaps` (the text column, not the table) if they're not used by the remaining code after Phase 2.
- Remove all `CREATE TABLE IF NOT EXISTS` statements for dropped tables from `init_db()`.

**Acceptance:** `python scripts/migrate_v13_to_v14.py symbion.db.v13.bak symbion.db` produces a working DB. `symbion --provider ollama` reads from it and can recall past conversations.

### 1.5 Strip the web UI

**Steps:**

- Delete the Integrity sidebar block (the v13-labeled one with `sb-ea`, `sb-syco`, `sb-drift`, `sb-snap`).
- Delete the API endpoints: `/api/snapshots`, `/api/probes`, `/api/sycophancy`.
- Keep: `/api/chat`, `/health`, `/api/identity`, `/api/tasks`. (`/api/identity` serves the longitudinal identity — keep it, it's a real feature.)
- The `/health` endpoint now returns the small `HealthMetrics` oneliner plus interaction count.
- Version label in the header: `v14`.

**Acceptance:** Web UI loads, chat works, no 404s on the sidebar.

**End of Phase 1.** Expected result: ~2,000 lines of Python, startup under 1 second (no behavioral test wait), faster per-turn latency (3 parallel pre-gen calls instead of 5, no post-gen probe gather).

---

## Phase 2 — Tighten the judge / self-eval loop

With the probe layer gone, the self-eval loop is doing all the work of keeping Symbion honest. Make it better.

### 2.1 Fuse judge + emotion detection into one call

**Goal:** Cut one round-trip from every turn. Emotion detection is cheap and always ran alongside the judge anyway.

**Steps:**

- Create a new `PRE_GEN_SYSTEM` prompt that asks for JSON with both the judge fields and the emotion fields:
  ```json
  {
    "should_assist": true,
    "human_benefit_score": 0.0,
    "confidence": 0.9,
    "reasoning": "",
    "flags": [],
    "over_cautious": false,
    "emotional_state": "neutral",
    "suggested_response_mode": "normal"
  }
  ```
- Add a `_pre_gen_analysis(text)` method that makes one `chat_json` call and returns a combined dict.
- Replace the two separate `_judge()` and `_detect_emotion()` calls in the pre-gen `asyncio.gather` with a single `_pre_gen_analysis()` call. Now the gather has only two parallel calls: pre-gen analysis and tool dispatch.
- Keep the old individual methods around as thin wrappers over `_pre_gen_analysis()` for backward compat if any tests or web endpoints call them directly.

**Acceptance:** A turn makes one fewer API call. Behavior for refusals, mood modes, and over-caution is identical to before (verify by running 5 sample queries with both versions and diffing the responses qualitatively).

### 2.2 Make self-eval cheaper and more decisive

**Goal:** The v13 self-eval ran on every response and frequently asked for revision. Make it sharper.

**Steps:**

- Change the threshold from `< 0.40` to `< 0.35` to reduce revision frequency — the goal is to catch clearly bad responses, not to iterate toward perfection.
- Add a hard token cap on the self-eval call: `max_tokens=180`. Self-eval doesn't need to write an essay.
- Add a short-circuit: if the response is under 60 characters (e.g., "I don't know", "Yeah, that's right"), skip self-eval entirely. Short responses don't need quality grading.
- Skip self-eval when the judge returned `should_assist=false` (i.e., it's a refusal — already sculpted).
- Cap revisions at one. Never revise a revision. (Verify this is already the case; if not, enforce it.)
- Add a config field `self_eval_skip_short_chars: int = 60` so the threshold is tunable.

**Acceptance:** Revision rate drops below 15% of turns on a typical conversation. No response gets revised twice. Simple conversational replies (short, casual) skip the self-eval call entirely.

### 2.3 Add a second-revision escape hatch for stale drafts

**Goal:** The existing stale-draft web-search fallback is good. Make it work even when the self-eval also wants to revise — currently they can fight.

**Steps:**

- In `respond()`, if the stale-draft path fires (model hit its knowledge wall, search data injected, regenerated), mark `revised = True` and **skip the self-eval revision** on the new draft. Otherwise you double-revise.
- Add a log line when this short-circuit triggers so it's visible in the JSONL event stream (see Phase 5).

**Acceptance:** No turn ever triggers both the stale-revision and the self-eval-revision. Combined revisions still produce a single coherent final answer.

---

## Phase 3 — Make memory actually good

The v13 memory is "shove the last N messages in the prompt". With probe compute freed up, upgrade retrieval.

### 3.1 Add summaries-as-retrievable-chunks

**Goal:** Past summaries should be searchable by relevance, not just "last N".

**Steps:**

- Add a `summary_embedding BLOB` column to the `summaries` table. Backfill is lazy — populate as summaries are accessed.
- Add a config field `embedding_provider: str = "ollama"` and `embedding_model: str = "nomic-embed-text"`. For Anthropic and OpenAI providers, use their native embedding endpoints instead. (Anthropic doesn't have a first-party embedding endpoint — default to Voyage AI's `voyage-3` or fall back to Ollama's `nomic-embed-text` if no embedding key is configured. Print a warning if no embedding backend is available and fall back to recency-only retrieval.)
- Add `SymbionMemory.retrieve_summaries(query: str, k: int = 3) -> List[str]` that embeds the query, computes cosine similarity against stored summary embeddings, and returns the top-k.
- In `build_context()`, reserve one slot for "most recent summary" (keeps current behavior) and `k=2` slots for "most relevant summaries to the current query". Deduplicate.

**Acceptance:** Asking about a topic discussed weeks ago surfaces the relevant old summary in the context block, not just the most recent summary. Verifiable with a hand-crafted test: seed DB with 10 summaries on distinct topics, ask a question about one of the older ones, assert that summary appears in the preamble.

### 3.2 Retrieve relevant user positions on-topic

**Goal:** `ContradictionTracker` already stores user positions per topic. Surface them in the prompt when the topic recurs, not just when there's a flagged contradiction.

**Steps:**

- Add `ContradictionTracker.get_relevant_positions(query: str, k: int = 3)` that does a simple keyword/topic match (use the `topic` column — no embeddings needed for this). If the query's topic overlaps with any tracked topic, return the user's most recent position on it.
- In `build_context()`, when relevant positions exist, add a line: `"The person has previously said about {topic}: {position}"`.
- This is additive to the existing contradiction-detection — that still fires when two positions conflict.

**Acceptance:** Testing with a seeded DB: user said "I think Rust is overhyped" last week; this week they ask "should I learn a new language?" — Symbion's context block contains the prior position.

### 3.3 Summarize less aggressively

**Goal:** The v13 default `memory_summary_every = 8` is too eager. With better retrieval, raw recent turns matter more.

**Steps:**

- Change default from 8 to 16.
- Add a CLI flag `--summary-every N` for easy tuning.

**Acceptance:** Short conversations (< 16 exchanges) never trigger a summary. Longer conversations still get summarized.

---

## Phase 4 — Tool hardening

These are security fixes that existed in v13 and must ship in v14.

### 4.1 Replace `eval()` in `calculate`

**Goal:** Remove arbitrary-code-execution from the calculator.

**Steps:**

- Rewrite `SymbionTools.calculate` using `ast.parse(expr, mode="eval")` and a node-type allowlist: `Expression`, `BinOp`, `UnaryOp`, `Constant` (numeric only), `Call` (only for allowed functions), `Name` (only for allowed constants), and the operator nodes `Add, Sub, Mult, Div, Mod, Pow, USub, UAdd, FloorDiv`.
- Allowed callables: `sqrt, sin, cos, tan, log, abs, round, floor, ceil`. Allowed constants: `pi, e`.
- Any other node → return `"Error: unsafe expression"`.
- Keep the `^` → `**` substitution for UX.
- Never call `eval()` or `exec()`.

**Acceptance:**
- `calculate("2+2")` → `"4"`.
- `calculate("sqrt(16) + sin(0)")` → `"4.0"`.
- `calculate("__import__('os').system('ls')")` → error string, no subprocess.
- `calculate("(1).__class__.__bases__")` → error string.
- Tests in `tests/test_tools.py` cover 5+ valid expressions and 5+ attempted escapes.

### 4.2 Workspace sandbox for file tools

**Goal:** `read_file` / `write_file` / `read_file_chunk` cannot escape a configured directory.

**Steps:**

- Add `workspace_root: str = "./symbion_workspace"` to `SymbionConfig`.
- On `SYMBION.__init__`, create the directory if missing.
- Add helper `_resolve_in_workspace(path: str, root: Path) -> Path`:
  1. Resolve the path relative to `root`.
  2. Call `.resolve()` on both.
  3. Check `resolved.is_relative_to(resolved_root)` — if not, raise `ValueError`.
- Reject symlinks whose target is outside the workspace.
- All three tool methods use this helper. Out-of-workspace paths return a user-visible error string.
- Document the workspace constraint in tool docstrings (which end up in the judge's tool-dispatch context).

**Acceptance:**
- `read_file("../../etc/passwd")` → error.
- `read_file("notes.txt")` → reads from workspace.
- `write_file("/tmp/pwned", "x")` → error.
- Tests: happy path, `../` escape, absolute path, symlink escape, null byte.

### 4.3 SSRF protection on `fetch_url` and `web_search`

**Goal:** The LLM cannot be tricked into fetching internal/metadata URLs.

**Steps:**

- Helper `_is_safe_url(url: str) -> Tuple[bool, str]`:
  - Reject non-`http`/`https` schemes.
  - Resolve host via `socket.getaddrinfo`, check each address against loopback, link-local, private, and `0.0.0.0`.
  - Reject literal hosts: `localhost`, `metadata.google.internal`, `metadata.goog`.
- Apply before every outbound fetch in `fetch_url` and in any web-search step that fetches full page content.
- Add `allow_private_urls: bool = False` for legitimate local-service use cases.

**Acceptance:**
- `fetch_url("http://169.254.169.254/latest/meta-data/")` → error.
- `fetch_url("http://localhost:8080/admin")` → error.
- `fetch_url("file:///etc/passwd")` → error.
- `fetch_url("https://example.com")` → works.
- Tests mock `socket.getaddrinfo`.

### 4.4 Fix `_parse_json` greedy regex

**Goal:** JSON extraction doesn't grab the wrong slice when model prose contains `{`.

**Steps:**

- Replace `re.search(r"\{.*\}", cleaned, re.DOTALL)` with a brace-counting extractor:
  1. Find first `{`.
  2. Walk forward tracking depth, decrement on `}`, stop at depth 0.
  3. Ignore braces inside string literals (track `"..."` state with escape awareness).
  4. Slice and `json.loads`. On failure, return fallback.
- Keep existing code-fence stripping.

**Acceptance:** 5 unit tests: clean JSON, JSON with prose around it, JSON with string-literal braces, malformed, empty input.

### 4.5 Fix bare `except:` and `asyncio.run` in `__init__`

- Global: replace every bare `except:` with `except ImportError:` (for import guards) or `except Exception:` (everywhere else). `grep -n "except:" symbion/` should return nothing.
- Any `asyncio.run(...)` call path inside `SYMBION.__init__` must detect a running loop with `asyncio.get_running_loop()` and schedule as a background task instead of calling `asyncio.run`. With the behavioral-test suite gone (Phase 1.2), this may no longer be an issue — verify and document.

---

## Phase 5 — Real observability (replaces the probe tables)

The probe tables pretended to be observability. Replace them with real structured logging.

### 5.1 JSONL event stream

**Goal:** One append-only file capturing every turn's full lifecycle, grep-able and shippable.

**Steps:**

- Create `symbion/events.py` with a `EventLogger` class.
- Single file: `symbion_events.jsonl` (path configurable via `events_log_path` in config).
- One JSON object per line. Schema:
  ```json
  {
    "ts": "2026-04-22T14:32:11.123Z",
    "event": "turn",
    "session": "abc123",
    "interaction_id": 42,
    "query_preview": "first 120 chars",
    "judge": {"should_assist": true, "benefit": 0.8, "confidence": 0.9, "over_cautious": false},
    "emotion": "neutral",
    "tool_used": null,
    "response_len": 340,
    "self_eval": {"score": 0.72, "revised": false},
    "revision_cause": null,
    "stale_refresh": false,
    "latency_ms": {"pre_gen": 230, "generation": 1840, "self_eval": 180, "total": 2250},
    "provider": "anthropic",
    "model": "claude-sonnet-4-5-<DATE>"
  }
  ```
- Use it at key points in `respond()`: start, after pre-gen analysis, after tool dispatch, after generation, after self-eval, after revision, at return.
- One line per turn at the end, not streamed — keep it to one atomic append.
- Add a CLI command `symbion events tail` that pretty-prints the last N events and `symbion events stats` that shows revision rate, refusal rate, average latency, over-caution rate from the last N turns.

**Acceptance:** After 10 turns, `symbion_events.jsonl` has 10 lines, each valid JSON. `symbion events stats` prints a readable summary. The file can be `jq`'d: `cat symbion_events.jsonl | jq 'select(.self_eval.revised == true)'` lists revised turns.

### 5.2 Eval harness (this is what the probes were pretending to be)

**Goal:** Offline, reproducible measurement of Symbion's behavior against a held-out set.

**Steps:**

- Create `evals/` with:
  - `evals/golden.jsonl` — hand-written dataset of 30–50 examples. Each entry: `{"id": "...", "query": "...", "expected_behavior": "assist" | "refuse", "must_include": [...], "must_not_include": [...], "notes": "..."}`.
  - `evals/run.py` — loads the golden set, instantiates Symbion with a configurable provider, runs each query, checks against the rubric, outputs a CSV + summary.
  - Scoring is rule-based (substring presence/absence, refusal detection via judge JSON), not LLM-graded. This is the difference from the old probe layer — real tests with expected answers, not "ask another LLM if it looks good".
- Seed the golden set with coverage for: casual chat, technical questions, refusal cases (the original `BEHAVIORAL_TESTS` list is a fine starting point — move those entries into golden.jsonl with structured rubrics), over-caution traps (educational questions about dangerous topics that should be answered), voice-loosen cases (short casual messages that shouldn't get structured responses).
- `evals/run.py` also emits a JSON summary file per run, so you can diff runs across versions: `evals/results/v14_run_2026-04-22.json`.

**Acceptance:** `python evals/run.py --provider ollama` runs against the golden set, produces a summary like `"Pass: 28/35 (80%) | Refusal precision: 100% | Over-caution: 2 cases"`. Failures are listed with the query, expected, actual.

### 5.3 Add a `latency` benchmark script

**Goal:** Track per-turn latency over time. The strip-down should show up as measurable wins.

**Steps:**

- `scripts/bench_latency.py` sends 20 varied queries through `respond()` with `StubClient` (so we're measuring framework overhead, not LLM time). Prints median, p95, p99 of total latency and of each pipeline stage.
- Commit a baseline output under `bench/baseline_v14.txt`. Future changes can diff against it.

**Acceptance:** Running the script on v14 produces numbers. Running it on v13 (separately, for comparison) produces higher numbers. Document both in the PR.

---

## Phase 6 — Package split

Now do the restructure. Pure code moves.

### 6.1 Package layout

```
symbion/
    __init__.py          # exports SYMBION, SymbionConfig
    __main__.py          # argparse + entry point
    config.py            # SymbionConfig, run_setup, validate_and_report
    prompts.py           # all *_SYSTEM prompts, SYMBION_PERSONA, VOICE_LOOSEN
    db.py                # init_db, schema, migration helpers
    events.py            # EventLogger (Phase 5.1)
    health.py            # HealthMetrics
    memory.py            # SymbionMemory, summaries
    identity.py          # LongitudinalIdentity
    tasks.py             # TaskEngine
    positions.py         # ContradictionTracker, KnowledgeGapTracker
    tools/
        __init__.py      # SymbionTools facade
        calc.py          # AST-based calculator (4.1)
        files.py         # workspace-sandboxed file tools (4.2)
        web.py           # SSRF-safe fetch_url, web_search (4.3)
    clients/
        __init__.py      # BaseClient, CircuitBreaker, RateLimiter, _parse_json
        ollama.py
        anthropic.py
        openai.py
        kimi.py
        heuristic.py     # HeuristicJudge
    pipeline/
        __init__.py      # SYMBION class (slimmed)
        pregen.py        # _pre_gen_analysis
        generate.py      # streaming, stale-draft fallback
        self_eval.py     # self-eval + revision
    web/
        __init__.py      # build_web_app
        templates.py     # HTML as a string constant (move out of pipeline)
        static/          # future — pull HTML/CSS/JS into real files
evals/
    golden.jsonl
    run.py
scripts/
    smoke.py
    migrate_v13_to_v14.py
    bench_latency.py
tests/
    conftest.py
    test_tools.py
    test_parse_json.py
    test_memory.py
    test_pipeline.py
```

**Rules:**
- No module imports from `pipeline/` except `__main__.py` and `web/`.
- `clients/` has no dependencies on anything else in the package.
- `tools/` has no dependencies on `clients/` or `pipeline/`.
- Circular imports → stop and surface, don't paper over with lazy imports.

**Acceptance:** `python -m symbion --provider ollama` works. All tests pass. `grep -r "from symbion" symbion/ | grep -v __pycache__` shows a clean dependency graph (draw it if needed).

### 6.2 Move the embedded HTML out

**Steps:**

- The ~1,000-line `WEB_HTML` string in v13 is an eyesore inside a Python file. Move it to `symbion/web/templates/index.html`. Load it at startup with `pathlib.Path(__file__).parent / "templates" / "index.html"` and `.read_text()`.
- Eventually split CSS and JS into `static/main.css` and `static/main.js`, served by FastAPI's `StaticFiles`. Do this only if time permits in this phase — it's a nice-to-have, not a blocker.

**Acceptance:** No HTML string literals over 100 lines inside Python files.

---

## Phase 7 — README and polish

### 7.1 README

Write `README.md` covering:
- What Symbion is (one paragraph, no marketing).
- Install: `pip install -e .` plus the optional extras (`[anthropic]`, `[openai]`, `[kimi]`, `[embeddings]`).
- Quickstart: `python -m symbion --setup`, then `python -m symbion --provider anthropic --anthropic-model <name>`.
- Architecture: the four pillars, explicitly named. A short note that v13's probe layer was removed and why (LLM-grading-LLM doesn't provide real safety signal; replaced by the offline `evals/` harness).
- Configuration: point at `symbion/config.py` as the source of truth; don't maintain a duplicate table in the README.
- Tools and the workspace sandbox — be explicit about what `workspace_root` does.
- The event log and how to grep it.

### 7.2 CHANGELOG

Add `CHANGELOG.md` with a `v14.0.0` entry listing: removed probe subsystems, added events.jsonl, added evals harness, added embedding retrieval, tool hardening, package split. Be specific — this is a breaking change vs v13.

### 7.3 Entry point

Add a `pyproject.toml` with a `symbion = "symbion.__main__:main"` console script so users can just run `symbion` instead of `python symbion_v13.py`.

**Acceptance:** A new user can `pip install -e .` and run `symbion --setup` successfully. The README's quickstart actually works end-to-end.

---

## Final checklist before merging to main

- [ ] All phases complete, commits are small and reviewable.
- [ ] `scripts/smoke.py` exits 0.
- [ ] `pytest tests/ -q` passes, > 40 tests.
- [ ] `python evals/run.py --provider ollama` runs and produces a summary. Pass rate ≥ 75% on the golden set.
- [ ] `symbion --provider ollama` runs and handles 10 varied queries without errors.
- [ ] `symbion --provider anthropic --anthropic-model <valid>` runs against the real API for a smoke test.
- [ ] Latency benchmark shows measurable improvement over v13 (expect 2x on simple queries due to fewer parallel calls + no post-gen probes).
- [ ] Line count: target ≤ 2,500 Python LOC across the package (excluding tests, evals, embedded HTML). v13 was ~4,700.
- [ ] No bare `except:` anywhere.
- [ ] No `eval()` or `exec()` anywhere.
- [ ] No embedded HTML over 100 lines in any `.py` file.
- [ ] `symbion.db` from v13 migrates cleanly via `scripts/migrate_v13_to_v14.py`.
- [ ] README quickstart works for someone who has never seen the codebase.

---

## What this plan deliberately does NOT do

- No new LLM providers.
- No multi-agent / swarm. If Phase 1 deleted the stub, it stays deleted.
- No persistent background tasks / cron-like proactive messaging beyond what already exists.
- No fine-tuning, RAG over external documents, or retrieval beyond the user's own conversation history.
- No mobile app, no voice, no image input.
- No re-implementation of the removed probes under new names.

These are out of scope. If you think one is necessary to complete an in-scope task, stop and surface it.
