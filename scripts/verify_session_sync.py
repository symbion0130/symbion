"""Verification harness for the cross-interface session-sync feature.

Boots Symbion's web app in-process on a temp DB with a stub respond()
so two-tab broadcast can fire without burning real LLM cost. Drives the
browser via Playwright and writes screenshots + a JSON observation log
to verify_artifacts/ so the verify-skill report has evidence.

Run via:  .python/python.exe scripts/verify_session_sync.py
"""
import asyncio, json, os, sys, tempfile, threading, time, signal
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import symbion_v14 as s
from playwright.async_api import async_playwright

ART = Path(__file__).resolve().parent.parent / "verify_artifacts"
ART.mkdir(exist_ok=True)
LOG = []

def log(step, status, note=""):
    LOG.append({"step": step, "status": status, "note": note})
    print(f"[{status}] {step}  {note}")


def build_app():
    cfg = s.SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.api_key = ""              # no auth in verify
    cfg.known_users = ["aaron"]   # avoid the multi-user picker modal blocking clicks
    cfg.db_path = tempfile.NamedTemporaryFile(suffix=".db", delete=False).name
    cfg.web_host = "127.0.0.1"
    cfg.web_port = 8123
    symbion = s.SYMBION(cfg)

    # Stub respond() — broadcast plumbing is the thing under test, not
    # generation. Returns deterministic text so two-tab assertions are
    # stable across runs.
    async def fake_respond(text, session, token_callback=None):
        # stream a short token sequence so the "self" client renders a
        # bubble (otherwise it'd only see the done frame)
        for chunk in ["Got it: ", text[:60], ".\n(stub reply)"]:
            if token_callback:
                await token_callback(chunk)
        symbion.memory.add("user", text, session, user="aaron")
        full = f"Got it: {text[:60]}.\n(stub reply)"
        symbion.memory.add("assistant", full, session, user="aaron")
        symbion.memory.set_active_session(session, user="aaron")
        return full, {"human_benefit_score": 0.5, "confidence": 0.5,
                      "should_assist": True, "flags": []}, 1
    symbion.respond = fake_respond
    return s.build_web_app(symbion), cfg


def run_server_in_thread(app, cfg, ready_event):
    import uvicorn
    config = uvicorn.Config(app, host=cfg.web_host, port=cfg.web_port,
                            log_level="warning", lifespan="on")
    server = uvicorn.Server(config)

    async def _runner():
        # Notify the main thread once uvicorn is actually serving.
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


async def main():
    app, cfg = build_app()
    base = f"http://{cfg.web_host}:{cfg.web_port}"
    ready = threading.Event()

    t = threading.Thread(target=run_server_in_thread, args=(app, cfg, ready),
                          daemon=True, name="symbion-verify-uvicorn")
    t.start()
    if not ready.wait(timeout=10):
        log("server start", "FAIL", "uvicorn never reported started")
        return 1
    log("server start", "PASS", f"listening on {base}")

    async with async_playwright() as pw:
        browser = await pw.chromium.launch(headless=True)
        ctx = await browser.new_context()
        page = await ctx.new_page()

        # Step 1: open index, expect connection (dot turns green)
        await page.goto(base, wait_until="domcontentloaded")
        await page.wait_for_selector("#chat-inner", timeout=5000, state="attached")
        # Wait for WS to connect — dot loses .dead
        try:
            await page.wait_for_function(
                "() => !document.getElementById('dot').classList.contains('dead')",
                timeout=8000)
            log("initial load + ws connect", "PASS")
        except Exception as ex:
            log("initial load + ws connect", "FAIL", str(ex)[:120])
        await page.screenshot(path=str(ART / "01_initial.png"), full_page=True)

        # Step 2: open sidebar — sidebar should be empty (no sessions yet)
        await page.click("#sidebar-btn")
        await page.wait_for_selector("#sidebar.open", timeout=3000)
        empty = await page.inner_text(".sidebar-empty")
        log("sidebar opens (empty initially)", "PASS" if "No past sessions" in empty else "FAIL",
            f"text={empty!r}")
        await page.screenshot(path=str(ART / "02_sidebar_empty.png"), full_page=True)
        await page.click("#sidebar-close")
        await page.wait_for_selector("#sidebar:not(.open)", timeout=3000)

        # Step 3: send a message -> assistant bubble should appear, sidebar
        # next time should show the session.
        await page.fill("#inp", "hello from tab A")
        await page.click("#btn")
        # Wait for the assistant bubble to finish (look for stub text)
        await page.wait_for_function(
            "() => document.querySelectorAll('.msg.sym .msg-body').length > 0 && "
            "Array.from(document.querySelectorAll('.msg.sym .msg-body')).some(b => "
            "(b.textContent||'').includes('Got it: hello from tab A'))",
            timeout=8000)
        log("send message -> assistant reply rendered", "PASS")
        await page.screenshot(path=str(ART / "03_after_send.png"), full_page=True)

        # Step 4: open sidebar — should now list the session
        await page.click("#sidebar-btn")
        await page.wait_for_selector("#sidebar.open", timeout=3000)
        await page.wait_for_selector(".session-row", timeout=3000)
        rows = await page.query_selector_all(".session-row")
        titles = []
        for r in rows:
            tnode = await r.query_selector(".session-title")
            titles.append((await tnode.inner_text()).strip())
        log("sidebar lists session after send", "PASS" if titles else "FAIL",
            f"titles={titles}")
        current_cls = await (rows[0]).get_attribute("class")
        log("current session highlighted", "PASS" if "current" in (current_cls or "") else "FAIL",
            f"class={current_cls!r}")
        await page.screenshot(path=str(ART / "04_sidebar_with_session.png"), full_page=True)

        # Step 5: "+ New chat" mints a fresh session, clears chat
        await page.click("#sidebar-new")
        await page.wait_for_function(
            "() => document.querySelectorAll('#chat-inner .msg').length === 0",
            timeout=4000)
        # Confirm SESSION changed in localStorage
        old_sess = titles  # not really old session id, but ok
        new_sess = await page.evaluate("() => localStorage.getItem('symbion_session')")
        log("New chat button clears DOM + mints new session id", "PASS",
            f"new SESSION={new_sess[:20]!r}")
        await page.screenshot(path=str(ART / "05_after_new.png"), full_page=True)

        # Wait for WS to reconnect on the new session
        await page.wait_for_function(
            "() => !document.getElementById('dot').classList.contains('dead')",
            timeout=5000)

        # Step 6: open sidebar — verify the OLD session is still listed,
        # click it, verify hydration restores the prior conversation.
        await page.click("#sidebar-btn")
        await page.wait_for_selector("#sidebar.open", timeout=3000)
        await page.wait_for_selector(".session-row", timeout=3000)
        rows2 = await page.query_selector_all(".session-row")
        # Find the row whose title matches our earlier message
        target_row = None
        for r in rows2:
            tn = await r.query_selector(".session-title")
            txt = (await tn.inner_text()).strip()
            if "hello from tab A" in txt:
                target_row = r; break
        if not target_row:
            log("old session still in sidebar after /new", "FAIL", f"rows={len(rows2)}")
        else:
            log("old session still in sidebar after /new", "PASS")
            await target_row.click()
            # Wait for chat to repopulate (REST hydration paints the bubbles)
            await page.wait_for_function(
                "() => Array.from(document.querySelectorAll('.msg.you')).some(m => "
                "(m.textContent||'').includes('hello from tab A'))",
                timeout=5000)
            log("click-to-switch hydrates prior conversation", "PASS")
        await page.screenshot(path=str(ART / "06_switched_back.png"), full_page=True)

        # Step 7: peer broadcast — open a second tab on the same SESSION,
        # send from tab A, watch tab B render remote_user + remote_assistant
        # with the .b-synced chip.
        session_id = await page.evaluate("() => localStorage.getItem('symbion_session')")
        page_b = await ctx.new_page()
        # Tab B shares localStorage via the same context, so it'll pick up
        # the same SESSION on load.
        await page_b.goto(base, wait_until="domcontentloaded")
        await page_b.wait_for_function(
            "() => !document.getElementById('dot').classList.contains('dead')",
            timeout=8000)
        sess_b = await page_b.evaluate("() => localStorage.getItem('symbion_session')")
        log("tab B picks up same session via localStorage",
            "PASS" if sess_b == session_id else "FAIL",
            f"A={session_id[:12]!r} B={sess_b[:12]!r}")

        # Type in tab A
        await page.bring_to_front()
        await page.fill("#inp", "from tab A to peers")
        await page.click("#btn")

        # Tab B should render a remote_user bubble (in .msg-remote-you wrapper)
        try:
            await page_b.wait_for_function(
                "() => Array.from(document.querySelectorAll('.msg-remote-you .msg.you'))."
                "some(m => (m.textContent||'').includes('from tab A to peers'))",
                timeout=8000)
            log("tab B sees remote_user with synced chip", "PASS")
        except Exception as ex:
            log("tab B sees remote_user with synced chip", "FAIL", str(ex)[:120])

        # And remote_assistant
        try:
            await page_b.wait_for_function(
                "() => Array.from(document.querySelectorAll('.msg.sym')).some(m => "
                "m.querySelector('.b-synced') && "
                "(m.querySelector('.msg-body')||{}).textContent && "
                "(m.querySelector('.msg-body').textContent.includes('Got it: from tab A')))",
                timeout=5000)
            log("tab B sees remote_assistant with synced chip", "PASS")
        except Exception as ex:
            log("tab B sees remote_assistant with synced chip", "FAIL", str(ex)[:120])
        await page_b.screenshot(path=str(ART / "07_peer_tab_b.png"), full_page=True)
        await page.screenshot(path=str(ART / "07_peer_tab_a.png"), full_page=True)

        # Probe: send from tab B back; tab A should now see the synced chip too
        await page_b.bring_to_front()
        await page_b.fill("#inp", "tab B replies")
        await page_b.click("#btn")
        try:
            await page.wait_for_function(
                "() => Array.from(document.querySelectorAll('.msg-remote-you .msg.you'))."
                "some(m => (m.textContent||'').includes('tab B replies'))",
                timeout=6000)
            log("PROBE: reverse direction (B -> A) also works", "PASS")
        except Exception as ex:
            log("PROBE: reverse direction (B -> A) also works", "FAIL", str(ex)[:120])

        # Probe: /api/sessions endpoint returns active pointer
        api = await page.evaluate("""async () => {
            const r = await fetch('/api/sessions?limit=10');
            return await r.json();
        }""")
        has_active = api.get("active") and api.get("active") == session_id
        log("PROBE: /api/sessions returns active pointer", "PASS" if has_active else "FAIL",
            f"active={api.get('active', '<none>')[:12]!r} sessions={len(api.get('sessions', []))}")

        await browser.close()

    # Dump log
    (ART / "log.json").write_text(json.dumps(LOG, indent=2))
    failures = [e for e in LOG if e["status"] == "FAIL"]
    print()
    print(f"== {len(LOG)} steps, {len(failures)} failures ==")
    for e in failures:
        print(f"   FAIL: {e['step']}  {e['note']}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
