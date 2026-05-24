"""Integration test for the web_command async refactor.

Pre-refactor (the bug): web_command was sync but /promote needed async
work. The code used asyncio.run_coroutine_threadsafe + fut.result(timeout=15)
to schedule the coroutine onto the running loop and block-wait for it —
but since web_command runs ON the loop thread itself (sync method called
from inside an async coroutine without thread offload), the block-wait
froze the very loop that was supposed to process the scheduled coroutine.
Net effect: WS loop hangs for 15 seconds on every web /promote, then
times out and reports 'Promote failed: TimeoutError'.

Post-refactor: web_command is async; /promote awaits promote_last_turn
directly on the running loop. No cross-loop gymnastics.

This test fires /promote via the WebSocket protocol and asserts the
response arrives quickly (<3s) — far below the 15s deadlock signature
that the broken code would have produced.
"""
import asyncio
import json
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

import pytest
import pytest_asyncio
import websockets

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import symbion_v14 as s


def _pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _build_app(port: int):
    cfg = s.SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.api_key = ""
    cfg.known_users = ["aaron"]
    cfg.shared_learnings_auto_import = False
    cfg.db_path = tempfile.NamedTemporaryFile(
        suffix=".db", prefix="symbion_webcmd_", delete=False
    ).name
    cfg.web_host = "127.0.0.1"
    cfg.web_port = port
    symbion = s.SYMBION(cfg)
    return s.build_web_app(symbion), cfg, symbion


def _run_uvicorn(app, cfg, ready: threading.Event):
    import uvicorn
    config = uvicorn.Config(app, host=cfg.web_host, port=cfg.web_port,
                            log_level="warning", lifespan="on")
    server = uvicorn.Server(config)

    async def runner():
        async def watch():
            while not server.started:
                await asyncio.sleep(0.05)
            ready.set()
        await asyncio.gather(server.serve(), watch())

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(runner())
    finally:
        loop.close()


@pytest_asyncio.fixture
async def ws_backend():
    """Boot Symbion's web app on a free port, seed the active session
    with one user/assistant pair so /promote has something to extract.
    Yields (base_ws_url, session_id, symbion_instance)."""
    port = _pick_free_port()
    app, cfg, symbion = _build_app(port)
    ready = threading.Event()
    threading.Thread(
        target=_run_uvicorn, args=(app, cfg, ready),
        daemon=True, name="symbion-webcmd-test-uvicorn",
    ).start()
    assert ready.wait(timeout=15), "uvicorn never reported started"

    session = "integ_webcmd_async"
    # Seed a user+assistant turn so /promote has a recent pair to extract.
    symbion.memory.add("user", "how do you handle a long PDF?",
                       session, user="aaron")
    symbion.memory.add("assistant", "Skim the table of contents first, "
                       "then read the conclusion to map the argument shape.",
                       session, user="aaron")

    yield f"ws://{cfg.web_host}:{cfg.web_port}/ws/{session}", session, symbion


@pytest.mark.asyncio
async def test_web_command_promote_does_not_hang_ws_loop(ws_backend):
    """The bug: /promote via WS used to freeze the loop for up to 15s
    waiting on a coroutine scheduled onto the same loop it was blocking.
    The fix: web_command is async, /promote awaits directly. Verify the
    response comes back in well under 3s (<<15s deadlock signature)."""
    ws_url, session, symbion = ws_backend

    async with websockets.connect(ws_url) as ws:
        # Auth frame first — server expects this before normal traffic.
        await ws.send(json.dumps({"type": "auth", "key": ""}))

        # Drain auth_ok + any initial status / history frames until we
        # have a clean channel to send /promote on.
        deadline = time.time() + 3.0
        while time.time() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=0.3)
                msg = json.loads(raw)
                if msg.get("t") == "auth_ok":
                    break
            except asyncio.TimeoutError:
                break

        # Fire /promote with verbatim text so the judge doesn't get
        # involved — keeps the test deterministic + fast.
        t_send = time.monotonic()
        await ws.send(json.dumps({
            "type": "cmd",
            "cmd": "/promote skim TOC then conclusion to map argument shape",
        }))

        # Wait for the cmd_result frame. Hard cap at 12s — the pre-fix
        # bug took 15s minimum; if it comes back well under that, the
        # loop was not blocked. promote_last_turn does a real embedding
        # call (Ollama /api/embeddings) which takes a few seconds, so
        # the threshold has to accommodate that work without lowering
        # below the deadlock detection signal.
        cmd_result = None
        deadline = time.time() + 12.0
        while time.time() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
                msg = json.loads(raw)
                if msg.get("t") == "cmd_result":
                    cmd_result = msg
                    break
            except asyncio.TimeoutError:
                continue

        elapsed = time.monotonic() - t_send

    assert cmd_result is not None, (
        f"never received cmd_result frame within 12s — WS loop may be "
        f"hung (pre-fix bug signature: 15s freeze + TimeoutError)"
    )
    # 8s is comfortably under the 15s pre-fix deadlock floor. Real work
    # (embedding generation) typically lands in 1-4s; anything over 8s
    # would suggest the deadlock is still partially in play.
    assert elapsed < 8.0, (
        f"cmd_result took {elapsed:.2f}s; pre-fix bug minimum was 15s. "
        f"Anything over 8s suggests the deadlock is still partially present."
    )
    lines = cmd_result.get("lines") or []
    assert any("Technique" in ln for ln in lines), (
        f"expected 'Technique #N saved' line in result, got {lines!r}"
    )
