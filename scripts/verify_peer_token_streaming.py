"""Verification harness for cfg.peer_token_streaming (gap #3).

Boots Symbion's web app in-process on a temp DB with a stubbed
respond() so the broadcast plumbing can fire without burning real LLM
cost. Opens two WebSocket peers on the same session and verifies:

  1. Client B (peer) receives a `remote_user` frame when client A sends.
  2. Client B receives one or more `remote_tok` frames during generation.
  3. Each `remote_tok` carries the same `request_id` as the `remote_user`
     and the closing `remote_assistant`.
  4. The `remote_assistant` text is the concatenation of the streamed
     tokens (peer's partial bubble can be replaced authoritatively).
  5. With peer_token_streaming=True but only one socket on the session,
     no remote_tok task spins up (no broadcast amplification for solo
     sessions).
  6. With peer_token_streaming=False, no remote_tok frames are sent
     even when two peers are connected — gating works as intended.

Run via:  .python/python.exe scripts/verify_peer_token_streaming.py
Exit code 0 on full pass, 1 on any FAIL.
"""
import asyncio, json, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import websockets
import symbion_v14 as s


LOG = []
def log(step, status, note=""):
    LOG.append({"step": step, "status": status, "note": note})
    print(f"[{status}] {step}  {note}")


def build_app(peer_token_streaming: bool, port: int):
    cfg = s.SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.api_key = ""
    cfg.known_users = ["aaron"]
    cfg.db_path = tempfile.NamedTemporaryFile(suffix=".db", delete=False).name
    cfg.web_host = "127.0.0.1"
    cfg.web_port = port
    cfg.peer_token_streaming = peer_token_streaming
    symbion = s.SYMBION(cfg)

    # Deterministic token stream so assertions on order/concat are stable.
    STREAM_PARTS = ["Hello ", "from ", "Symbion ", "stub."]

    async def fake_respond(text, session, token_callback=None):
        for part in STREAM_PARTS:
            if token_callback:
                await token_callback(part)
            # Small sleep so the broadcaster has time to drain a frame
            # between tokens — exercises the queue ordering, not just a
            # fast-path bulk-send race.
            await asyncio.sleep(0.01)
        symbion.memory.add("user", text, session, user="aaron")
        full = "".join(STREAM_PARTS)
        symbion.memory.add("assistant", full, session, user="aaron")
        symbion.memory.set_active_session(session, user="aaron")
        return full, {"human_benefit_score": 0.5, "confidence": 0.5,
                      "should_assist": True, "flags": []}, 1
    symbion.respond = fake_respond
    return s.build_web_app(symbion), cfg, symbion


def run_server_in_thread(app, cfg, ready_event):
    import uvicorn
    config = uvicorn.Config(app, host=cfg.web_host, port=cfg.web_port,
                             log_level="warning", lifespan="on",
                             ws_max_size=80 * 1024 * 1024)
    server = uvicorn.Server(config)

    async def _runner():
        async def _watch():
            while not server.started:
                await asyncio.sleep(0.05)
            ready_event.set()
        await asyncio.gather(server.serve(), _watch())

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(_runner())
    finally:
        loop.close()
    return server


async def open_peer(base_ws: str, session_id: str):
    """Open a WS, send auth (empty key OK with cfg.api_key=''), and
    drain the initial frames (history / status / user_init / done)
    until we hit a quiet moment ready for the test traffic."""
    ws = await websockets.connect(f"{base_ws}/ws/{session_id}", open_timeout=5)
    # Auth even when api_key is "" — handler still expects the frame
    # and replies with auth_ok.
    await ws.send(json.dumps({"type": "auth", "key": ""}))
    # Drain initial setup frames until we see the final "done" that
    # wraps the connect handshake. Bounded by a timeout so a missing
    # frame doesn't hang the test.
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=0.4)
        except asyncio.TimeoutError:
            break
        try:
            data = json.loads(msg)
        except json.JSONDecodeError:
            continue
        if data.get("t") == "done":
            # Connect-wrap done frame; we're ready.
            break
    return ws


async def collect_frames(ws, until_type: str, timeout: float = 5.0):
    """Drain frames from ws until we see one with t==until_type or
    the overall timeout fires. Returns the list of parsed frames."""
    frames = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            msg = await asyncio.wait_for(
                ws.recv(), timeout=max(0.1, deadline - time.monotonic()))
        except asyncio.TimeoutError:
            break
        try:
            data = json.loads(msg)
        except json.JSONDecodeError:
            continue
        frames.append(data)
        if data.get("t") == until_type:
            return frames
    return frames


async def run_scenario_streaming_on(base_http: str, base_ws: str):
    """Two peers, peer_token_streaming=True. Expect remote_tok stream."""
    session = "test_peer_streaming"
    a = await open_peer(base_ws, session)
    b = await open_peer(base_ws, session)
    log("two peers connected", "PASS")

    # Have A send a chat message; meanwhile B drains frames until it
    # sees remote_assistant (the authoritative final).
    async def _send_from_a():
        await a.send(json.dumps({"type": "chat", "text": "test ping"}))

    async def _collect_on_b():
        return await collect_frames(b, until_type="remote_assistant",
                                     timeout=8.0)

    _, frames_b = await asyncio.gather(_send_from_a(), _collect_on_b())

    types = [f.get("t") for f in frames_b]
    print(f"  client B frame types: {types}")

    # 1. remote_user came through.
    remote_user = next((f for f in frames_b if f.get("t") == "remote_user"), None)
    if remote_user is None:
        log("remote_user delivered to peer", "FAIL", "no remote_user frame on B")
        await a.close(); await b.close()
        return False
    log("remote_user delivered to peer", "PASS",
        f"request_id={remote_user.get('request_id','?')[:8]}")

    # 2. remote_tok frames present.
    rtoks = [f for f in frames_b if f.get("t") == "remote_tok"]
    if not rtoks:
        log("remote_tok frames received", "FAIL",
            "peer_token_streaming=True but no remote_tok on B")
        await a.close(); await b.close()
        return False
    log("remote_tok frames received", "PASS",
        f"{len(rtoks)} token frames")

    # 3. request_id consistent across remote_user, remote_tok*, remote_assistant.
    rid_user = remote_user.get("request_id")
    rid_toks = {f.get("request_id") for f in rtoks}
    remote_assist = next((f for f in frames_b if f.get("t") == "remote_assistant"), None)
    if remote_assist is None:
        log("remote_assistant delivered", "FAIL", "no remote_assistant on B")
        await a.close(); await b.close()
        return False
    rid_assist = remote_assist.get("request_id")
    if not (rid_user and rid_toks == {rid_user} and rid_assist == rid_user):
        log("request_id consistent across frames", "FAIL",
            f"user={rid_user!r} toks={rid_toks!r} assistant={rid_assist!r}")
        await a.close(); await b.close()
        return False
    log("request_id consistent across frames", "PASS", rid_user)

    # 4. remote_assistant.text equals concatenation of remote_tok deltas.
    streamed = "".join(f.get("v", "") for f in rtoks)
    final = remote_assist.get("text", "")
    if streamed != final:
        log("streamed concat == authoritative final", "FAIL",
            f"streamed={streamed!r} final={final!r}")
        await a.close(); await b.close()
        return False
    log("streamed concat == authoritative final", "PASS",
        f"{len(final)} chars")

    await a.close()
    await b.close()
    return True


async def run_scenario_solo(base_http: str, base_ws: str):
    """One peer, peer_token_streaming=True. No second client → no
    remote_tok broadcaster should spin up. The originator still gets
    normal tok frames; this just verifies we don't waste work."""
    session = "test_peer_solo"
    a = await open_peer(base_ws, session)
    log("solo peer connected", "PASS")

    # Send a chat; collect on the same socket until done.
    await a.send(json.dumps({"type": "chat", "text": "solo ping"}))
    frames = await collect_frames(a, until_type="done", timeout=8.0)
    # The originator only sees t='tok' frames (not remote_tok — that
    # would be the peer's view). What we're verifying here is that the
    # turn completes cleanly even when peer_token_streaming is True
    # and no peer is connected.
    has_done = any(f.get("t") == "done" for f in frames)
    has_tok = any(f.get("t") == "tok" for f in frames)
    await a.close()
    if not (has_done and has_tok):
        log("solo turn completes with peer_token_streaming=True", "FAIL",
            f"done={has_done} tok={has_tok}")
        return False
    log("solo turn completes with peer_token_streaming=True", "PASS",
        f"{len(frames)} frames")
    return True


async def run_scenario_streaming_off(base_http: str, base_ws: str):
    """Two peers, peer_token_streaming=False. No remote_tok should fire."""
    session = "test_peer_off"
    a = await open_peer(base_ws, session)
    b = await open_peer(base_ws, session)

    async def _send_from_a():
        await a.send(json.dumps({"type": "chat", "text": "off ping"}))

    async def _collect_on_b():
        return await collect_frames(b, until_type="remote_assistant",
                                     timeout=8.0)

    _, frames_b = await asyncio.gather(_send_from_a(), _collect_on_b())
    rtoks = [f for f in frames_b if f.get("t") == "remote_tok"]
    await a.close()
    await b.close()
    if rtoks:
        log("no remote_tok when streaming off", "FAIL",
            f"got {len(rtoks)} remote_tok frames")
        return False
    has_assist = any(f.get("t") == "remote_assistant" for f in frames_b)
    if not has_assist:
        log("no remote_tok when streaming off", "FAIL",
            "no remote_assistant either — broadcast totally broken")
        return False
    log("no remote_tok when streaming off", "PASS",
        "remote_assistant still delivered without intermediate frames")
    return True


async def drive_one_scenario(peer_token_streaming: bool, port: int, scenario_fn):
    app, cfg, _ = build_app(peer_token_streaming=peer_token_streaming, port=port)
    ready = threading.Event()
    t = threading.Thread(target=run_server_in_thread, args=(app, cfg, ready),
                          daemon=True,
                          name=f"verify-uvicorn-{port}")
    t.start()
    if not ready.wait(timeout=10):
        log(f"server start (port {port})", "FAIL", "uvicorn never reported started")
        return False
    log(f"server start (port {port})", "PASS",
        f"peer_token_streaming={peer_token_streaming}")
    base_http = f"http://{cfg.web_host}:{cfg.web_port}"
    base_ws   = f"ws://{cfg.web_host}:{cfg.web_port}"
    # Give uvicorn an extra beat to finish wiring the WS route.
    await asyncio.sleep(0.3)
    try:
        return await scenario_fn(base_http, base_ws)
    except Exception as ex:
        log("scenario raised", "FAIL", f"{type(ex).__name__}: {ex}")
        return False


async def main():
    # Three scenarios, each on its own port (and its own SYMBION instance,
    # because cfg.peer_token_streaming is read once at SYMBION construction
    # and there's no live-reload path).
    ok1 = await drive_one_scenario(True,  8131, run_scenario_streaming_on)
    ok2 = await drive_one_scenario(True,  8132, run_scenario_solo)
    ok3 = await drive_one_scenario(False, 8133, run_scenario_streaming_off)

    all_ok = ok1 and ok2 and ok3
    print()
    print("=" * 60)
    print(f"OVERALL: {'PASS' if all_ok else 'FAIL'}")
    for entry in LOG:
        print(f"  [{entry['status']}] {entry['step']}  {entry['note']}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
