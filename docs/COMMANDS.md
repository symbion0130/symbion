# Symbion commands

Cheat sheet of every command line you'll actually use. Default workflow most of the time: `.\symbion --web` → use it → `Ctrl+C` when done. Everything below is for the edge cases.

---

## At the PowerShell prompt (`PS D:\symbion>`)

| Command | What it does |
|---|---|
| `.\symbion` | Terminal REPL (default, no web) |
| `.\symbion --web` | Web UI on http://localhost:8000 + LAN URL in the banner |
| `.\symbion --kill` | Cleanly shut down a running `--web` server (reads `SYMBION_API_KEY` from `.env`) |
| `.\symbion --setup` | Re-do API key setup interactively |
| **Ctrl+C** (inside `--web`) | Clean shutdown in the **same** terminal — same code path as `--kill` |

Useful flags that stack with the above:

| Flag | Default | Effect |
|---|---|---|
| `--port N` | 8000 | Different web port |
| `--host 127.0.0.1` | 0.0.0.0 | Localhost only, no LAN exposure |
| `--provider anthropic\|openai\|ollama\|kimi` | anthropic | Switch responder |
| `--think` | off | Show chain-of-thought trace in responses |
| `--proactive 30` | 0 | Unprompted messages every N minutes |
| `--no-tools` / `--no-eval` / `--no-agent-loop` | all on | Disable subsystems for debugging |
| `--save-config` | — | Persist current flag set to `symbion.json` and exit |

---

## Inside the terminal REPL (`you >`)

Full list lives in `/help`. The ones you'll actually use:

| Command | What it does |
|---|---|
| `/quit` | Save summary + exit |
| `/think` | Toggle reasoning-trace display |
| `/summarize` | Flush the current session to long-term memory without quitting |
| `/forget` / `/forget <topic>` | Wipe session memory, or selectively wipe by topic |
| `/tasks`, `/identity`, `/gaps` | Show internal state slices |
| `/tool-stats` | Per-tool reliability counters this session |
| `/escalate` | Force the **next** turn through the stronger model (Opus 4.7) |
| `/whoami` | Symbion's self-description |
| `/paste` | Enter multi-line paste mode (end with `///`) |
| `/provider anthropic\|kimi` | Switch responder provider at runtime |
| `/feedback <id> <-1.0..1.0> [comment]` | Rate a past interaction by `iid` |

---

## Inside the web UI composer (iPhone or browser)

Much smaller set than terminal:

| Command | What it does |
|---|---|
| `/quit` or `/end` | Save & end the session, start a fresh one (server flushes a summary first) |
| `/new` | Start a fresh session **without** saving (current chat is discarded from view; messages stay in DB) |
| `/key` | Re-open the API key modal to change or clear it |
| `/help` | Print this list inline |

You can also tap the **key** button in the web header to change the API key.

---

## Cleanup / admin

| Need | How |
|---|---|
| Run only the eval suite | `D:\symbion\.python\python.exe evals/run.py --provider anthropic --concurrency 4` |
| Smoke test the responder | `D:\symbion\.python\python.exe scripts/smoke.py` |
| Run pytest | `D:\symbion\venv\Scripts\python.exe -m pytest tests/ -q` |
| Force a OneDrive push without quitting Symbion | `D:\symbion\.python\python.exe scripts/sync.py push` |
| Check sync status | `D:\symbion\.python\python.exe scripts/sync.py status` |
| Compile-check the codebase | `D:\symbion\.python\python.exe -m py_compile symbion_v14.py` |

---

## Which Python is which

The repo has three Python interpreters, used for different jobs:

- **`D:\symbion\.python\python.exe`** — portable embeddable Python 3.12 with all production deps (httpx, fastapi, uvicorn, mcp, etc.). Used by `.\symbion` and `.\symbion.bat`. This is the canonical interpreter for running Symbion itself.
- **`D:\symbion\venv\Scripts\python.exe`** — standard venv that also has pytest. Used for running the test suite.
- **System Python** (`C:\Users\Aaron Henson\AppData\Local\Programs\Python\Python312\python.exe`) — bare interpreter, missing httpx and some deps. Mostly avoid.

---

## Shutdown semantics (so the noise stays gone)

- `Ctrl+C` inside `.\symbion --web`: caught by Python, prints `OK Symbion shut down.`, runs the OneDrive push via the `atexit` hook, exits clean. No traceback, no `Terminate batch job (Y/N)?` prompt.
- `.\symbion --kill` from another shell: posts to `/api/shutdown`, which raises SIGINT internally — same path as Ctrl+C, same clean exit.
- `/quit` typed in the terminal REPL: writes a session summary, falls out of the REPL loop, hits the same `atexit` push on the way out.
- `/quit` or `/end` typed in the web composer: server flushes a summary for the current session, then the UI auto-rolls to a fresh `session_id` (stored in localStorage). The page does NOT reload; the chat scroll clears and a new conversation begins.
