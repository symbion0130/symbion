# Changelog

## v14.0.0 — 2026-04-22

### Removed
- **Probe subsystems** (11 classes): EvalAwarenessProbe, SandbaggingProbe, RewardHackDetector, AdversarialRedTeam, SnapshotDriftTracker, BehavioralProbeEngine, SycophancyDetector, DeceptionProbe, SituationalAwarenessProbe, FrameAcceptanceProbe, SchemingProbe
- **KimiSwarmCoordinator** (multi-agent orchestration stub)
- **SurvivalMetrics** and **SymbionSurvivalInstinct** (replaced by HealthMetrics)
- **Survival gate** in respond() — only the judge can refuse now
- **20-test behavioral startup check** — instant startup
- **14 DB tables** for probe logging
- **22 config fields**, **11 CLI flags**, **14 terminal commands**, **10 web API endpoints** related to probes
- **Integrity sidebar** from web UI

### Added
- **HealthMetrics** — telemetry-only metrics (no gate), keeps mood() for persona prompt
- **PRE_GEN_SYSTEM** — fused judge + emotion detection in one LLM call (saves one round-trip per turn)
- **AST-based calculator** — replaces eval() with ast.parse + node allowlist
- **Workspace sandbox** — file tools restricted to ./symbion_workspace
- **SSRF protection** — _is_safe_url() blocks private IPs, metadata endpoints, non-HTTP schemes
- **Brace-counting JSON extractor** — replaces greedy regex in _parse_json
- **EventLogger** — JSONL event stream (symbion_events.jsonl) for per-turn telemetry
- **Eval harness** (evals/) — 30-entry golden set, rule-based scoring, no LLM grading
- **Latency benchmark** (scripts/bench_latency.py)
- **DB migration script** (scripts/migrate_v13_to_v14.py)
- **Package structure** — symbion/__init__.py, __main__.py for `python -m symbion`
- **External HTML template** — web UI moved from inline string to symbion/web/templates/index.html

### Changed
- **Pre-gen gather**: 2 parallel calls (fused judge+emotion, tool dispatch) instead of 3-5
- **Self-eval threshold**: lowered from 0.40 to 0.35 (fewer unnecessary revisions)
- **Self-eval short-circuit**: responses under 60 chars skip eval entirely
- **Stale-draft escape**: skip self-eval revision when stale-draft already revised (no double-revision)
- **Memory summary interval**: default changed from 8 to 16
- **Cross-session retrieval**: build_context() now surfaces relevant past summaries and user positions
- All bare `except:` replaced with `except ImportError:` or `except Exception:`
- Line count: 4728 -> 2564 (46% reduction, excluding external HTML)

### Breaking changes
- Config fields removed — old symbion.json files with probe fields will load fine (unknown keys ignored), but code referencing them will break
- `self.survival.metrics` -> `self.health` throughout
- DB schema: 14 tables dropped — use scripts/migrate_v13_to_v14.py to migrate
