"""Boot animation: present on cold load, fades after backend-ready,
skipped on reload-within-session.

Follows scripts/verify_session_sync.py's server-in-thread pattern but
runs under pytest so the integration suite has one command. No real
LLM calls — the animation gates on /health which the web layer serves
synchronously off the FastAPI app, not the responder.
"""
import asyncio
import socket
import sys
import tempfile
import threading
from pathlib import Path

import pytest
import pytest_asyncio

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import symbion_v14 as s
from playwright.async_api import async_playwright


def _pick_free_port() -> int:
    """Pick an ephemeral port the OS confirms is free. Avoids the test
    colliding with a real Symbion already serving on 8000."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _build_app(port: int):
    cfg = s.SymbionConfig()
    cfg.llm_provider = "ollama"               # no key required
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.api_key = ""
    cfg.known_users = ["aaron"]
    cfg.shared_learnings_auto_import = False
    cfg.db_path = tempfile.NamedTemporaryFile(
        suffix=".db", prefix="symbion_splash_", delete=False
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
async def web_server():
    """Boot Symbion's FastAPI app in a daemon thread on a free port.
    Yields the base URL. Teardown lets the daemon die with the process."""
    port = _pick_free_port()
    app, cfg, symbion = _build_app(port)
    ready = threading.Event()
    threading.Thread(
        target=_run_uvicorn, args=(app, cfg, ready),
        daemon=True, name="symbion-splash-test-uvicorn",
    ).start()
    assert ready.wait(timeout=15), "uvicorn never reported started"
    yield f"http://{cfg.web_host}:{cfg.web_port}"


@pytest.mark.asyncio
async def test_splash_present_then_fades(web_server):
    """Cold-load: overlay paints with display!=none, then within 5s
    (3250ms hold + ~450ms fade + slack for slow CI) acquires the
    'done' class which sets display:none. fadeReason should be
    'health-ok' since the test backend serves /health immediately."""
    async with async_playwright() as pw:
        browser = await pw.chromium.launch(headless=True)
        ctx = await browser.new_context()
        page = await ctx.new_page()
        try:
            await page.goto(web_server, wait_until="domcontentloaded")
            # Overlay should be present and NOT yet .done at first paint.
            splash = page.locator("#boot-splash")
            await splash.wait_for(state="attached", timeout=2000)
            initially_done = await splash.evaluate(
                "el => el.classList.contains('done')"
            )
            assert not initially_done, (
                "boot-splash was already .done on first paint — "
                "sessionStorage bled across contexts, or the gate fired early"
            )
            # All 7 SVG pieces present.
            for cls in ("bm-disc", "bm-ring", "bm-top", "bm-bot",
                        "bm-dotL", "bm-rng", "bm-dotR"):
                count = await page.locator(f"#boot-splash .{cls}").count()
                assert count == 1, f"expected one .{cls}, got {count}"
            # Wait for .done — fade should fire on first /health 200,
            # then 450ms transition, then the transitionend listener.
            await page.wait_for_function(
                "() => document.getElementById('boot-splash')"
                "  .classList.contains('done')",
                timeout=10000,
            )
            reason = await splash.get_attribute("data-fade-reason")
            assert reason == "health-ok", (
                f"expected fade reason 'health-ok', got {reason!r}. "
                f"'max-wait' would mean /health never responded — "
                f"check the FastAPI app + the gate logic."
            )
        finally:
            await ctx.close()
            await browser.close()


@pytest.mark.asyncio
async def test_splash_skipped_on_reload_within_session(web_server):
    """sessionStorage gate: second load in the SAME browser context
    (i.e. reload) should skip the animation entirely. Splash is .done
    on first paint."""
    async with async_playwright() as pw:
        browser = await pw.chromium.launch(headless=True)
        ctx = await browser.new_context()
        page = await ctx.new_page()
        try:
            # First load: splash plays. Wait for it to finish so the
            # sessionStorage flag is set before we reload.
            await page.goto(web_server, wait_until="domcontentloaded")
            await page.wait_for_function(
                "() => document.getElementById('boot-splash')"
                "  .classList.contains('done')",
                timeout=10000,
            )
            # Reload — splash should be .done immediately, no animation.
            await page.reload(wait_until="domcontentloaded")
            splash = page.locator("#boot-splash")
            await splash.wait_for(state="attached", timeout=2000)
            done_now = await splash.evaluate(
                "el => el.classList.contains('done')"
            )
            assert done_now, (
                "expected boot-splash.done immediately on reload "
                "(sessionStorage gate didn't fire)"
            )
        finally:
            await ctx.close()
            await browser.close()


@pytest.mark.asyncio
async def test_splash_replays_in_fresh_context(web_server):
    """A fresh browser context has empty sessionStorage, so the splash
    plays again. Confirms the gate is per-window, not global."""
    async with async_playwright() as pw:
        browser = await pw.chromium.launch(headless=True)
        # First context — splash plays.
        ctx1 = await browser.new_context()
        try:
            page1 = await ctx1.new_page()
            await page1.goto(web_server, wait_until="domcontentloaded")
            await page1.wait_for_function(
                "() => document.getElementById('boot-splash')"
                "  .classList.contains('done')",
                timeout=10000,
            )
        finally:
            await ctx1.close()
        # Second context — splash should play again from scratch.
        ctx2 = await browser.new_context()
        try:
            page2 = await ctx2.new_page()
            await page2.goto(web_server, wait_until="domcontentloaded")
            splash = page2.locator("#boot-splash")
            await splash.wait_for(state="attached", timeout=2000)
            initially_done = await splash.evaluate(
                "el => el.classList.contains('done')"
            )
            assert not initially_done, (
                "fresh context's splash was .done on first paint — "
                "sessionStorage bled across contexts (shouldn't)"
            )
        finally:
            await ctx2.close()
            await browser.close()
