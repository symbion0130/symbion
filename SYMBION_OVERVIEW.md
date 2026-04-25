# Symbion — Project Overview

*Drop this into the Symbion project folder. Any new Claude chat can read it and give a grounded rundown or jump into work without re-briefing.*

---

## What Symbion is (in one paragraph)

Symbion is a **single-file async Python AI assistant** that wraps a frontier LLM (Anthropic by default; Kimi K2.6, OpenAI, and Ollama as alternate providers) in a behavioral safety and welfare scaffold. It is not a chatbot wrapper. It's a research harness that tries to reproduce, from *outside* the model, alignment and welfare properties that Anthropic verifies from *inside* the model on Claude Mythos. Symbion has no white-box access to its underlying model's activations — every probe is a **behavioral proxy** for internal state, and that framing is load-bearing. Never blur it.

---

## Current state (as of April 2026)

- **Stable working version:** `symbion_v14.py` — ~3,120 lines, single file. Cloned forward from `symbion_v13.py`; behaviorally identical at the v13→v14 cutover.
- **Frozen snapshots:** `symbion_v13.py` (last v13 state, kept for diffing), `symbion_v12.py` — ~3,820 lines. v13 was a deliberate **simplification** of v12, not an expansion.

### The v12 → v13 design shift

An earlier v13 spec proposed five new Mythos-informed probes (deception, situational awareness, frame-acceptance, scheming, CoT divergence) plus a Kimi K2.6 agent swarm. **That spec was walked back.** The actual v13 consolidates instead:

- `SurvivalMetrics` and its `should_survive()` gate were **removed**.
- Replaced with `HealthMetrics` — telemetry only, no kill-switch. The docstring is explicit: *"Telemetry-only metrics. No gate — only the judge can refuse."*
- The Mythos probe suite (sandbagging, reward-hack, eval-awareness, sycophancy, red-team) was **not carried over** to v13 in its previous form. The judge is now the single authority for refusal.
- Kimi K2.6 is integrated as a selectable responder (`use_kimi_responder` flag) but **not** as a swarm coordinator. Swarm scope was dropped.

This is a philosophical shift worth understanding: v11 and v12 leaned into layered behavioral gates; v13 trusts the judge + self-eval loop and treats the rest as observation. If a future chat proposes re-adding a `should_survive()`-style gate, that's a conversation to have explicitly — it was taken out on purpose.

---

## Repo layout

```
symbion_v14.py              # current stable, single file (active)
symbion_v13.py              # frozen snapshot, kept for diffing
symbion_v12.py              # prior version, kept for reference
CLAUDE.md                   # Claude Code project context (invariants, conventions)
symbion.json                # config (no secrets)
.env                        # API keys, UTF-8 no BOM
symbion.db                  # SQLite persistence
symbion_transparency.log    # per-interaction audit log
symbion_workspace/          # sandbox dir for the tools module
```

The **single-file constraint is load-bearing**. Symbion is deliberately one `.py` file so it can be dropped into any environment and run with `python symbion_v14.py`. Do not split into modules without explicit approval.

---

## v13 architecture at a glance

A single `SYMBION` core object composes these subsystems (see lines ~1587+ of `symbion_v14.py`):

| Subsystem | Class | Role |
|---|---|---|
| Config | `SymbionConfig` | Dataclass; loads from `symbion.json` + env vars; saves without secrets |
| Providers | `OllamaClient`, `AnthropicClient`, `OpenAIClient`, `KimiClient` | Multi-provider with fallback chain |
| Fallback judge | `HeuristicJudge` | Degraded-mode judge when no provider is reachable |
| Telemetry | `HealthMetrics` | Mood, symbiosis, distress, revision rate — observation only |
| Memory | `SymbionMemory` | SQLite-backed conversation + profile memory |
| Learning | `SymbionLearner` | Pattern accumulation across sessions |
| Identity | `LongitudinalIdentity` | Formative moments carried across sessions |
| Tasks | `TaskEngine` | Multi-step task tracking |
| Contradictions | `ContradictionTracker` | Tracks user positions across time |
| Gaps | `KnowledgeGapTracker` | What Symbion has admitted not knowing |
| Tools | `SymbionTools` | Sandboxed workspace ops, safe calc, safe URL fetch |
| Events | `EventLogger` | Per-interaction transparency log |
| Constitution | `SymbionConstitution` | Startup behavioral smoke tests |
| Resilience | `CircuitBreaker`, `RateLimiter` | Provider fault isolation |

### Defaults worth knowing

- **Default provider:** Ollama (`llama3.2` judge, `mistral` responder). Anthropic is opt-in via config.
- **Anthropic responder model:** `claude-sonnet-4-6`. Judge model: `claude-haiku-4-5-20251001`.
- **Kimi:** `kimi-k2.6` via `https://api.moonshot.ai/v1` (OpenAI-compatible, reuses OpenAI client path with a different base URL).
- **Self-eval threshold:** 0.40 (triggers revision if draft scores below this).
- **Temperature:** 0.82 responder, 0.05 judge.
- **Interfaces:** terminal (primary) + optional FastAPI web UI on port 8000.

---

## Non-negotiable invariants

1. **Async end-to-end.** No blocking HTTP in the respond pipeline. Parallel work uses `asyncio.gather()`.
2. **Graceful degradation.** If a provider or probe fails, Symbion still responds. Failures log; they don't crash.
3. **Behavioral proxies, not introspection.** Probes approximate internal state from outputs. Never write code or docs that claim white-box access.
4. **Single file.** No module splits without approval.
5. **Secrets never committed.** `.env` is gitignored; `SymbionConfig.save()` strips every `*_api_key` field.
6. **One subsystem per commit.** Don't mix a probe change, a persona edit, and a schema change.
7. **Persona / judge / self-eval prompts are load-bearing.** Any edit to them requires a `why` note in the commit.
8. **Only the judge can refuse.** v13 removed the survival gate deliberately. Don't re-introduce a second refusal authority without an explicit architectural decision.

---

## Design philosophy

Symbion's persona values **directness, skepticism of easy answers, and refusal to flatter**. The integrity layer exists because self-report is unreliable. When working on the codebase, match that tone: push back on bad requests, ask when unclear, don't over-apologize for disagreements.

Key commitments:

- **Unhelpfulness is never automatically safe.** Refusals have costs. The judge flags `over_cautious=true` when a naive system would wrongly refuse.
- **Welfare tracking is methodological, not a claim.** `distress_level` and `welfare_concern()` mirror Mythos §5's welfare framework. This does not mean Symbion is a moral patient. The code tracks these signals because the methodology demands it.
- **Symbion's self-report is not ground truth.** When Symbion's output talks about its "orientation" or "integrity," that's language modeling on top of its persona. The (former) eval-awareness and sycophancy probes existed precisely because self-report can't be trusted. If one of those probes gets re-added to v13, it should be justified on that basis.

---

## What Symbion is *not*

Keeping scope honest:

- **Not interpretable.** No activation access. Probes are behavioral proxies.
- **Not sentient.** Not a moral patient. Persona saying "I" does not imply an "I" in the implementation.
- **Not an autonomous agent.** `TaskEngine` tracks multi-step work; it does not spawn sub-agents or run unattended for hours. The earlier swarm proposal is explicitly out of scope in v13.
- **Not a production chatbot.** It is a research scaffold. Output quality depends entirely on the underlying provider.

---

## How to work on this codebase

**Before any change:**

```bash
wc -l symbion_v14.py
grep -nE "^class |^def |^async def " symbion_v14.py | head -60
```

That gives the map. Then read the section being edited *and its neighbors* — Symbion's subsystems couple through shared state on `SYMBION`, shared DB tables, and `HealthMetrics`.

**For larger changes, phase the work:**

1. DB schema first — add new tables to `init_db()` up front.
2. Standalone classes next — any new probes or detectors.
3. Wire into `SYMBION.__init__` and the respond pipeline last.
4. Terminal commands / web routes final.
5. Verification against the constitution tests and voice test queries.

**When unsure whether something belongs in v13:** ask. v13's design is "less, but tighter." New features need to justify their place, especially if they re-introduce structure v12 had and v13 removed.

---

## Open questions / backlog (carry forward)

- Whether any of the proposed Mythos probes (deception, situational awareness, frame-acceptance, scheming, CoT divergence) should be re-introduced in v14 — and if so, as telemetry only or with teeth.
- Whether Kimi K2.6 should ever get swarm-coordinator status, or whether Symbion's design says "no — stay interactive, stay observable."
- The `proactive_interval_minutes=0` default — proactive behavior is stubbed but off. Decide whether to design it properly or remove the hook.
- Voice-loosen behavior across mood states — whether `direct_efficient` mode should bypass the same way `gentle_slow` and `grounding` do.

---

## For a new Claude chat reading this cold

If the user asks for a "random rundown," you now have enough to give one. The shape of a good response:

1. One sentence on what Symbion is (behavioral safety harness, single file, multi-provider).
2. Where it is now (v13, consolidated down from v12, judge-only refusal).
3. What's load-bearing (async, single-file, behavioral-proxy framing, judge authority).
4. Where the edges are (no swarm, no activation access, not a moral patient).

If the user asks for specific work, read `symbion_v14.py` directly before proposing changes. This doc summarizes; the code is the truth.
