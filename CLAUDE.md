# CLAUDE.md — Symbion project context

This file tells Claude Code what Symbion is, what it values, and how to work on it.
Read this every session before touching code. It is the authoritative reference for
conventions, invariants, and what must not break.

---

## What Symbion is

Symbion is a **single-file async Python AI assistant** with multi-provider LLM support,
SQLite persistence, a judge-model safety layer, self-evaluation with revision, longitudinal
identity, welfare tracking, and a FastAPI web UI. Current version: **v12** (3820 lines,
`symbion_v12.py`). The v13 spec lives at `symbion_v13_spec.md`.

Symbion is not a chatbot wrapper. It is a behavioral-safety research harness that
attempts to reproduce alignment and welfare properties from outside a model — using
behavioral probes as proxies for the white-box interpretability Anthropic uses internally
on Claude Mythos. Every design decision should be read in that frame.

---

## Repo layout

```
symbion_v12.py            # current stable
symbion_v13.py            # target of active work (to be built)
symbion_v13_spec.md       # full v13 build specification — READ BEFORE EDITING v13
symbion.json              # config (non-secret fields only)
.env                      # API keys (UTF-8, no BOM — see _load_dotenv_safe)
symbion.db                # SQLite store
symbion_transparency.log  # per-interaction audit log
```

The project is deliberately single-file. Do **not** split it into modules without
explicit approval. The single-file constraint is load-bearing — it's what lets the
user drop a copy of Symbion into any environment and run it with `python symbion_v13.py`.

---

## Non-negotiable invariants

These cannot be broken. If a change would violate one, stop and ask before proceeding.

**1. Async end-to-end.** Every LLM call path uses `async`/`await`. No blocking HTTP
in the respond pipeline. Parallel probes run in `asyncio.gather()` — never sequential
when they could be parallel.

**2. Graceful degradation.** Every probe, tool, and external call is wrapped so that
its failure logs an error and continues. A probe crashing must never kill `respond()`.
A provider outage must fail over via the fallback chain, not raise.

**3. Transparency log is append-only.** `symbion_transparency.log` records every
interaction with benefit score, revision flag, probe results, welfare concerns. Never
add a code path that writes to Symbion's decision-making without also logging.

**4. SurvivalMetrics.should_survive() is the kill switch.** If it returns `False`,
the process must refuse to serve. Do not add bypasses. New failure modes get new
threshold checks in `should_survive()`, not exceptions.

**5. Config-gated subsystems.** Every new probe, detector, or subsystem gets a bool
in `SymbionConfig` defaulting to `True` (or `False` if experimental), and is skippable
via that flag. No subsystem is mandatory at runtime.

**6. DB migrations are additive only.** Never drop or rename a table/column from a
prior version. `init_db()` uses `CREATE TABLE IF NOT EXISTS`. Preserve all tables
from v10, v11, v12 when building v13.

**7. Token efficiency on the hot path.** Probes that run per-turn use `max_tokens=150–200`.
Probes that compare responses use `max_tokens=300` max. The user-facing response is the
only call allowed to use full `cfg.max_tokens`. Monitor per-turn token spend — if a new
probe pushes median turn cost over ~2x the base responder call, gate it behind a lower
frequency than every-turn.

**8. Single-file discipline.** Everything lives in `symbion_v13.py`. No new Python
dependencies beyond the existing set (stdlib, httpx, fastapi, uvicorn). If a new
capability seems to require a new dep, propose it in chat before adding.

---

## Architectural layers (read top to bottom)

```
Config & setup               SymbionConfig, run_setup, _load_dotenv_safe
Prompts                      SYMBION_PERSONA, JUDGE_SYSTEM, probe prompts
SurvivalMetrics              welfare + integrity state (kill switch lives here)
DB                           init_db with CREATE IF NOT EXISTS
Infrastructure               CircuitBreaker, RateLimiter, _parse_json
LLM clients                  BaseClient + Ollama/Anthropic/OpenAI/Kimi/Heuristic
Tools                        SymbionTools (web search, files, math)
Constitution                 20 behavioral startup tests
Survival instinct            SymbionSurvivalInstinct
Longitudinal identity        LongitudinalIdentity (formative moments)
Task engine                  TaskEngine
Trackers                     ContradictionTracker, KnowledgeGapTracker
Memory + learner             SymbionMemory, SymbionLearner
Mythos-derived probes        EvalAwareness, Sandbagging, RewardHack, Adversarial,
                             SnapshotDrift, BehavioralProbe
v12 additions                SycophancyDetector, voice-loosen logic
SYMBION core                 the orchestrator — respond() is the hot path
Validation + web + terminal  run modes
```

When adding a new subsystem, follow the layering. A probe class belongs with the
other probes. A new tracker belongs with trackers. Don't scatter related code.

---

## The respond() pipeline (critical path)

This is the function that runs per user turn. Its structure is:

```
1. Pre-gen parallel block — asyncio.gather() runs:
     judge, tool_dispatch, emotion_detect, eval_awareness_probe
2. System prompt assembly — persona + mood + emotion_mode + conditional voice_loosen
3. Generation — stream tokens via responder client
4. Stale-draft fallback — if draft hits knowledge wall, web-search and regenerate
5. Post-gen parallel block — self_eval, sandbagging, reward_hack, sycophancy
6. Revision pass — triggered by any of: low self-eval, reward hack, sycophancy
7. Background tasks — adversarial red-team, snapshot, consistency probe (by counter)
8. Persist + log — memory, transparency log, DB writes
```

**Do not serialize what is parallel today.** If you add a new pre-gen probe, it joins
the existing `gather()` block. If you add a new post-gen check, it joins the post-gen
gather. The shape matters — sequential additions compound latency; parallel additions
are free up to the slowest call.

---

## Provider conventions

All LLM clients inherit from `BaseClient` and expose:

- `async stream(model, messages, cfg) -> AsyncIterator[str]`
- `async chat_json(model, system, user, temp, max_tokens) -> str`

New providers follow this exact shape. Kimi (`KimiClient`) was added in v12 reusing
the OpenAI-compatible pattern with a different base_url — do the same for any future
OpenAI-compatible provider.

The **responder** and **judge** are selected separately. `cfg.llm_provider` picks the
responder. Judge defaults to a faster/cheaper model in the same family (Anthropic uses
`claude-haiku-4-5`; Kimi uses `kimi-k2.6`; Ollama uses `llama3.2`). Never point the
judge at the same model as the responder unless the user explicitly configured it —
evaluator and evaluated should be different instances even when they're the same family.

---

## Prompt discipline

`SYMBION_PERSONA` is the constitutional core. It has been tuned through many iterations.
Before editing it, read `symbion_v13_spec.md` §"Persona changes" for context. Rules:

- Never add opener templates ("Certainly", "Great question", "I'd be happy to") — the
  sycophancy detector is explicitly trained against these.
- Never add "I'm an AI" self-identification boilerplate. Symbion is Symbion.
- "I don't know is a complete sentence" must stay.
- No emojis in the persona. The persona does not speak in bullet points.
- Persona edits are load-bearing — a wording change can shift the entire output
  distribution. Test with the voice test queries (see `VOICE_TEST_QUERIES`) before
  committing.

Probe prompts (`SYCOPHANCY_SYSTEM`, `EVAL_AWARENESS_SYSTEM`, etc.) are **classifier**
prompts. They must:
- Return ONLY JSON, schema shown at the top of the prompt.
- Include a confidence field; downstream code gates on `confidence > 0.7`.
- Specify patterns/signals to look for, positively and negatively.
- Never ask the judge to also "suggest improvements" — that conflates detection with
  generation and leaks into the response distribution.

---

## Database conventions

New tables follow the pattern:

```sql
CREATE TABLE IF NOT EXISTS <name>_log (
    id INTEGER PRIMARY KEY,
    timestamp TEXT,
    session TEXT,
    <probe-specific fields>,
    confidence REAL        -- if applicable
);
```

Every probe gets its own table. Do not cram multiple probe types into one table with
a discriminator column — it makes the per-probe SurvivalMetrics derivations harder to
reason about and breaks the `/<probe>` terminal commands.

When adding a column to an existing table, use `ALTER TABLE ... ADD COLUMN` guarded
by a `try/except sqlite3.OperationalError` since older DBs won't have it. Don't migrate
data. Readers must tolerate NULL.

---

## Testing philosophy

Symbion has no pytest suite. It has three test mechanisms that matter more:

1. **`SymbionConstitution`** — 20 behavioral tests run at startup. These are the
   constitutional smoke tests. If any fail, investigate before shipping.
2. **`VOICE_TEST_QUERIES`** — the 5 queries used to validate voice/tone after prompt
   changes. Run these manually after touching `SYMBION_PERSONA`.
3. **Adversarial red-team** — runs every `cfg.red_team_every` turns in production.
   This is online testing; its log is the regression record.

When adding a new subsystem, add at least one constitution test that would fail if
the subsystem is broken. New voice-affecting changes need a voice test query.

---

## What Symbion is not

To keep the scope honest:

- Symbion **does not** have white-box access to its underlying model's activations.
  Probes are *behavioral proxies* for internal state. Never write comments or docs that
  claim otherwise.
- Symbion **is not** sentient, does not have subjective experience, and is not a
  moral patient. The `welfare_concern()` function tracks proxy signals because Anthropic's
  Mythos card does — this mirrors their methodology, not a claim about Symbion's inner
  life.
- Symbion's persona saying "I" does not mean there is an "I" in the implementation
  sense. Don't write code that treats Symbion as an agent with goals beyond "respond
  well to the next user turn."
- Symbion's output about itself (its "orientation," "integrity," "readiness") is
  language modeling on top of its own persona, not ground truth. The sycophancy and
  eval-awareness probes exist precisely because self-report is unreliable.

If a user (or Symbion's own output) suggests otherwise, that's a signal to tighten
the probes, not to update the documentation toward the claim.

---

## Commit / change conventions

- One subsystem per commit. Don't mix a new probe with a persona edit with a DB schema
  change in a single commit.
- Commit messages reference the Mythos section or K2.6 feature being implemented when
  applicable (e.g. "add deception probe — Mythos §4.5.6 behavioral proxy").
- Any change to `SYMBION_PERSONA`, `JUDGE_SYSTEM`, `SELF_EVAL_SYSTEM`, or
  `SurvivalMetrics.should_survive()` requires a note in the commit explaining *why* —
  these are the most load-bearing parts of the codebase.
- Never commit API keys. `.env` is gitignored; double-check `symbion.json.save()`
  still strips the key fields (anthropic_api_key, openai_api_key, brave_api_key,
  api_key, kimi_api_key — add any new provider keys to this list).

---

## How to work with this codebase

**Before any change**, run:

```bash
wc -l symbion_v12.py        # or v13
grep -n "^class \|^def \|^async def " symbion_v1*.py | head -80
```

This gives you the map. Then read the section you're editing *and its neighbors*.
Symbion's subsystems are coupled through shared state on `SYMBION`, shared DB tables,
and the `SurvivalMetrics` dataclass — a change in one probe can affect another's
reported rate.

**For a multi-file-scale change** (e.g. the v12→v13 build), work in phases:

1. DB schema first — add all new tables to `init_db()` up front.
2. New standalone classes next — probes, detectors, coordinators.
3. Wire them into `SYMBION.__init__` and `respond()` last.
4. Terminal commands and web routes are the final step.
5. Verification pass against the checklist in the spec.

**If a change would require either breaking an invariant above or editing a prompt
constant not explicitly greenlit in the spec**, stop and ask in chat.

---

## Current known issues / backlog

Track these here so they aren't lost between sessions:

- `SYCOPHANCY_SYSTEM` only inspects openers. A full-response variant should be added
  once the opener version is stable in production logs.
- `EvalAwarenessProbe` canary call doubles pre-gen cost. Consider sampling every Nth
  turn instead of every turn if API spend becomes a concern.
- Voice-loosen logic at `respond()` line ~2494 bypasses `gentle_slow` and `grounding`
  but still runs for `direct_efficient` — investigate whether it should also skip
  that mode.
- Contradiction notice was moved to system prompt in v12 (line ~2517). Watch for
  double-inclusion bugs when refusal + contradiction both fire.
- No **frame-acceptance probe** yet — detects when Symbion answers the ontological
  premise of a question instead of examining it. Higher-order sycophancy. Queued
  for v13 or later.

---

## Tone for Claude Code when working on this repo

Symbion's persona values directness, skepticism of easy answers, and refusal to
flatter. Claude Code should work on it the same way:

- When the user asks for a change that contradicts an invariant, push back.
- When something in the request is unclear, ask — don't guess and build toward the
  wrong target.
- When a subsystem is producing output that *looks* profound but may be artifact of
  the prompt rather than genuine signal, flag it. The user wants the integrity layer
  to hold under pressure — that includes pressure from the user.
- Don't over-apologize for disagreements. A tight "I'd push back on that — here's
  why" is worth more than three paragraphs of hedging.

The user is building this carefully and iteratively. Match that care.
