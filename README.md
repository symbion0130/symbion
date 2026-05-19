# Symbion v14

Symbion is an async Python AI assistant with multi-provider LLM support, SQLite persistence, a judge-model safety layer, self-evaluation with revision, longitudinal identity, and a FastAPI web UI. It is a behavioral-safety research harness that uses behavioral probes as proxies for internal state — not a chatbot wrapper.

## Install

See **[docs/SETUP.md](docs/SETUP.md)** for the full setup guide — four install paths (Worker one-liner, portable drive, system Python, dev clone), API keys, optional add-ons (Ollama, MCP, OCR, sqlite-vec), and a gotchas table.

TL;DR:

```powershell
# Fresh Windows machine — Worker one-liner (clone + bootstrap + key seed in one paste)
irm https://symbion-installer.symbion-0130.workers.dev?t=<INSTALL_TOKEN> | iex
# Locked-down machine variant if the above errors with "running scripts is disabled":
powershell -ExecutionPolicy Bypass -Command "irm https://symbion-installer.symbion-0130.workers.dev?t=<INSTALL_TOKEN> | iex"
```

```bash
# System Python (dev clone)
pip install -e .[web]
python -m symbion --setup        # interactive: writes API keys to .env
python -m symbion --web          # http://localhost:8000

# Portable drive (no system Python required)
scripts\bootstrap-portable.bat   # one-time, ~5 min
symbion                          # terminal REPL (default) — same as scripts\start.bat
symbion --web                    # web UI on http://localhost:8000
```

## Architecture

Four pillars:

1. **Persona** (`SYMBION_PERSONA`, voice-loosen logic) — constitutional identity
2. **Memory & identity** — SQLite-backed conversation memory, user profile, longitudinal identity, task engine, contradiction tracker, knowledge gap tracker
3. **Judge -> Responder -> Self-eval loop** — pre-gen judge+emotion call (fused), generation with stale-draft fallback, post-gen quality eval with single-shot revision
4. **Tools** — AST-based calculator, workspace-sandboxed file I/O, SSRF-protected web search and URL fetch

v14 removed the probe layer (11 LLM-grading-LLM subsystems) that ran in v11-v13. These added latency and cost without providing real safety signal. They are replaced by:
- **Offline eval harness** (`evals/`) with a golden set of 30 test cases, scored rule-based
- **JSONL event stream** (`symbion_events.jsonl`) for real-time telemetry

## Configuration

Source of truth: `symbion_v14.py`, class `SymbionConfig`. Key fields:

- `llm_provider`: "ollama" | "anthropic" | "openai" | "kimi"
- `self_eval_enabled` / `self_eval_threshold`: quality gating
- `voice_loosen_enabled`: casual-tone adaptation
- `tools_enabled`: calculator, file I/O, web search
- `memory_summary_every`: turns between auto-summaries (default 16)

Config persists to `symbion.json` via `--save-config`. API keys live in `.env` (never in JSON).

## Tools and the workspace sandbox

File tools (`read_file`, `write_file`) are sandboxed to `./symbion_workspace/`. Paths that escape via `../` or absolute paths are rejected. Symlinks targeting outside the workspace are also blocked.

The calculator uses AST validation — no `eval()` on untrusted input. Only numeric expressions with allowed functions (sqrt, sin, cos, tan, log, abs, round, floor, ceil) and constants (pi, e).

URL fetching rejects non-HTTP schemes, localhost, cloud metadata endpoints, and private/loopback IPs.

## Event log

Every turn appends one JSON line to `symbion_events.jsonl`:
```bash
cat symbion_events.jsonl | jq 'select(.self_eval.revised == true)'
```

## Eval harness

Run offline against the golden set:
```bash
python evals/run.py --provider ollama
```

Results are saved to `evals/results/` as JSON summaries for diffing across versions.

## DB migration from v13

```bash
python scripts/migrate_v13_to_v14.py symbion.db.v13.bak symbion_v14.db
```

This copies the 12 kept tables and drops the 14 probe-related tables.
