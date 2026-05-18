# Symbion setup guide

How to go from a fresh machine to a running Symbion. Three paths depending on what you have on hand.

---

## Quickest path: portable drive (no system Python needed)

For when Symbion lives on an external drive and you want to plug it into any x64 Windows machine.

```cmd
:: From the drive root (e.g. D:\symbion). Works in both PowerShell and cmd.
scripts\bootstrap-portable.bat   :: one-time, ~5 min, ~100MB
symbion                          :: terminal REPL (default)
symbion --web                    :: web UI on http://localhost:8000
```

`bootstrap-portable.bat` downloads Python 3.12 embeddable into `.python\` on the drive, installs pip + setuptools, then runs `pip install -e .[web] mcp mcp-server-time pypdf`. After that, `symbion` (a thin top-level shortcut to `scripts\start.bat`) launches Symbion using **only** the drive's Python — system Python (if any) is irrelevant.

Terminal is the default so the drive works the same on every machine you plug it into: open PowerShell at the drive root, run `symbion`, talk to it. Add `--web` (or any other flag) when you want the browser UI. Extra args pass straight through to `python -m symbion`, so `symbion --provider anthropic`, `symbion --web --port 9000`, etc. all work. `scripts\start.bat` still exists and does the same thing if you prefer the explicit path.

Use this when:
- You want one drive that "just works" across multiple machines
- The target machine has no admin rights for installing Python system-wide
- You don't want to pollute the target machine's site-packages

Skip this when:
- You only ever run on one machine — system Python is simpler

---

## Standard path: system Python

```bash
git clone https://github.com/<you>/symbion.git
cd symbion
pip install -e .[web]
```

Required: Python ≥ 3.10. Tested on 3.11, 3.12, 3.14.

That installs `httpx`, `fastapi`, `uvicorn[standard]` and registers Symbion in editable mode. Then:

```bash
python -m symbion --setup       # writes API keys to .env interactively
python -m symbion --web         # starts the web UI on :8000
# or
python symbion_v14.py --provider anthropic   # terminal REPL
```

---

## API keys

Symbion needs **at least one** LLM provider. Keys go in `.env` (gitignored — never check them in). `--setup` walks you through it interactively, or write the file manually:

```ini
ANTHROPIC_API_KEY=sk-ant-...
OPENAI_API_KEY=sk-...
KIMI_API_KEY=...
BRAVE_API_KEY=...        # optional, for higher-quality web_search
```

You don't need all of them. The default `symbion.json` ships with `llm_provider: anthropic`, so an Anthropic key alone is enough for the full stack (responder + judge both go through Anthropic — Sonnet 4.6 + Haiku 4.5).

---

## Optional add-ons

Install only the ones you'll use. All are gated by soft imports — Symbion runs fine without any of them.

| Capability | Install | When to add |
|---|---|---|
| **PDF reading** | `pip install pypdf` | `read_pdf` tool |
| **OCR fallback for scanned PDFs** | `pip install pypdfium2 pytesseract` + Tesseract binary on PATH ([UB-Mannheim installer](https://github.com/UB-Mannheim/tesseract/wiki)) | When PDFs have no extractable text |
| **Ollama (local responder/judge/embeddings)** | Install Ollama, then `ollama pull mxbai-embed-large` | Local-only mode, paraphrase retrieval, or offline development |
| **MCP servers** | `pip install mcp mcp-server-time` (and others) | Connect Symbion to external tools via Model Context Protocol |
| **sqlite-vec index** | `pip install sqlite-vec` | Only worth installing once you have 1000+ summaries — at smaller scale the full-scan path is faster |

### Ollama notes

Ollama is the only way to enable **cosine embedding retrieval** (`mxbai-embed-large`, 1024 dims). Without it, Symbion falls back to BM25 only — you keep lexical precision but lose paraphrase matching. The fallback is silent and graceful.

If you're on a RAM-constrained machine (≤4 GB free), skip Ollama entirely. Cloud responder + BM25 retrieval is plenty.

### MCP servers

Symbion supports stdio-transport MCP servers. Add entries to `symbion.json`:

```json
"mcp_enabled": true,
"mcp_servers": [
  {
    "name": "time",
    "command": "python",
    "args": ["-m", "mcp_server_time"],
    "enabled": true
  }
]
```

Tools surface as `mcp__<server>__<tool>` to avoid colliding with built-ins. **MCP is only active in `--web` mode** — terminal mode's per-turn `asyncio.run()` can't sustain subprocess sessions across loops.

Verify it works end-to-end:
```bash
python scripts/mcp_check.py
```

---

## Configuration

Two files:

- **`.env`** — secrets only (API keys). Loaded once at startup. Never committed.
- **`symbion.json`** — everything else. Loaded on startup, written by `--save-config`. Safe to commit (`config.save()` strips API key fields automatically).

Key fields with sensible defaults:

| Field | Default | Notes |
|---|---|---|
| `llm_provider` | `"anthropic"` | `anthropic`, `openai`, `ollama`, `kimi`, `hf_router`, `deepseek` |
| `anthropic_model` | `claude-sonnet-4-6` | Responder |
| `anthropic_judge_model` | `claude-haiku-4-5-20251001` | Pre-gen judge, self-eval, profile, summarisation |
| `max_tokens` | `8192` | Responder cap. Drop to `~4000` on Ollama models with smaller context |
| `memory_summary_every` | `8` | Turns between auto-summaries |
| `proactive_interval_minutes` | `0` | Set >0 for unprompted messages (needs real judge) |
| `embedding_enabled` | `true` | Set `false` if Ollama isn't available and you don't want startup probes |
| `tools_enabled` | `true` | Master switch for the 10 built-in tools |
| `mcp_enabled` | `true` | Empty `mcp_servers` list = no-op |

---

## Verify it works

After install + config, run the smoke test:

```bash
python scripts/smoke.py
```

Should report `Smoke test PASSED` with a real response from your configured responder. Failure modes:

- `(Generation error: ReadTimeout)` — Ollama with a model that doesn't fit Symbion's system prompt (mistral's 8K context is too tight; use `llama3.2` or `phi4`)
- `Provider : OFFLINE-STUB` — no LLM configured; check `.env` and `symbion.json`
- `Smoke test FAILED: responder returned no usable text` — see above

Full test suite (51 tests, ~3s):

```bash
python -m pytest tests/ -q
```

---

## Windows: speed up local Python and file I/O

Optional. On older laptops without a discrete GPU, Windows Defender real-time scanning is the dominant overhead on Python startup, pytest, and git operations — it re-scans files on every read/write, and the Python interpreter touches hundreds of files per launch.

**Run in an elevated PowerShell** (right-click → Run as Administrator). Adjust the path if Symbion lives somewhere other than `D:\symbion`:

```powershell
# Exclude the project tree + the Python interpreters that read from it.
Add-MpPreference -ExclusionPath    "D:\symbion"
Add-MpPreference -ExclusionProcess "python.exe"
Add-MpPreference -ExclusionProcess "D:\symbion\.python\python.exe"
Add-MpPreference -ExclusionProcess "D:\symbion\venv\Scripts\python.exe"

# Verify
Get-MpPreference | Select-Object -ExpandProperty ExclusionPath
Get-MpPreference | Select-Object -ExpandProperty ExclusionProcess
```

**Optionally, stop Windows Search from indexing the project tree.** Lightest touch: Control Panel → Indexing Options → Modify → uncheck the Symbion directory. Heavier (only if you don't use Start-menu file search):

```powershell
# Elevated.
Stop-Service  WSearch
Set-Service   WSearch -StartupType Disabled
```

**Don't bother with OneDrive on-demand toggling** for the project — Symbion's repo lives outside any OneDrive directory by default. The cross-machine DB sync via `scripts\sync.py` is explicit and unaffected.

Expected impact on a CPU-only laptop: Python startup and pytest wall-time both drop noticeably (commonly 30–50%). No effect on Anthropic API latency or local Ollama inference — those are bottlenecks elsewhere.

---

## Common gotchas

| Symptom | Cause | Fix |
|---|---|---|
| `.bat` script aborts with `'.' was unexpected at this time` | Nested parens in cmd `IF` block | Should be fixed in current scripts — re-pull if you see it |
| `WARNING: No supported WebSocket library` | Bare `uvicorn` instead of `uvicorn[standard]` | `pip install 'uvicorn[standard]'` |
| `Cannot import 'setuptools.build_meta'` on portable Python | Embeddable Python doesn't ship setuptools | `bootstrap-portable.bat` installs it explicitly — re-run if you're on an older copy |
| Terminal mode says "MCP servers configured but only active in web mode" | Expected | Run with `--web` to use MCP |
| `database is locked` under web load | Pre-WAL builds | Should be fixed since the WAL commit; verify `PRAGMA journal_mode` returns `wal` |
| 184s "Generation error" with empty message | Model context exceeded (Ollama + mistral) | Switch responder to `llama3.2` or `phi4` |
| Vec index warnings about UNIQUE constraint | sqlite-vec 0.1.9 quirk on re-add | Should be fixed in current code (DELETE-then-INSERT pattern) |

---

## Where to go next

- **`CLAUDE.md`** — architecture, invariants, working style. Read before editing.
- **`scripts/`** — `smoke.py`, `mcp_check.py`, `bench_latency.py`, `bootstrap-portable.bat`, `start.bat`
- **`evals/golden.jsonl`** — regression set (63 entries, 7 buckets)
- **`symbion_events.jsonl`** — per-turn telemetry. `jq` it to debug what the judge saw, what tools fired, etc.
- **Terminal commands** — `/help` for the full list. Useful: `/tool-stats`, `/mcp`, `/forget <topic>`, `/consolidate`
