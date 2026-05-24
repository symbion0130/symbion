"""Integration-test fixtures: temp DB + temp event log + real provider.

Pattern mirrors evals/run.py's isolation: every test gets a fresh tempfile
DB and tempfile events log so the real symbion.db / symbion_events.jsonl
are never read or polluted. Tests skip cleanly when the required API key
is absent (CI without secrets, fresh-machine checkout) instead of failing.

Unlike tests/conftest.py's StubClient, these fixtures boot SYMBION with
real LLM clients. That means tests cost real money / rate budget per run.
Keep the test set small and the queries short.
"""
import json
import os
import sys
import tempfile
from pathlib import Path

import pytest
import pytest_asyncio

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from symbion_v14 import SymbionConfig, SYMBION, EventLogger


def _base_cfg() -> SymbionConfig:
    """SymbionConfig with everything that would touch real state disabled.

    self_eval off: post-gen quality review is slow (~2-3s) and not part of
    what these tests cover. proactive scheduler off: no daemon threads.
    shared_learnings auto-import off: don't read OneDrive in tests. mcp off:
    no external server spawn.
    """
    cfg = SymbionConfig()
    cfg.tools_enabled = True
    cfg.agent_loop_enabled = True
    cfg.self_eval_enabled = False
    cfg.proactive_interval_minutes = 0
    cfg.shared_learnings_auto_import = False
    cfg.mcp_enabled = False
    # Isolated DB per test. delete=False because Windows refuses to delete
    # an open file; cleanup happens in the fixture teardown.
    cfg.db_path = tempfile.NamedTemporaryFile(
        suffix=".db", prefix="symbion_integ_", delete=False
    ).name
    return cfg


def _build_symbion(cfg: SymbionConfig):
    """Construct SYMBION + redirect EventLogger to a temp file we can read
    back. Returns (symbion, events_path) — caller owns cleanup of both."""
    sym = SYMBION(cfg)
    events_path = tempfile.NamedTemporaryFile(
        suffix=".jsonl", prefix="symbion_events_", delete=False
    ).name
    sym.events = EventLogger(events_path)
    return sym, events_path


def _cleanup_paths(*paths):
    for p in paths:
        try:
            os.unlink(p)
        except OSError:
            pass


@pytest_asyncio.fixture
async def symbion_groq():
    """Symbion wired to Groq as primary provider. Cheap + fast (~1.2s p50).
    Used by tests that exercise the basic respond() pipeline without
    needing native tool use (Groq's supports_tools=False)."""
    cfg = _base_cfg()
    if not cfg.groq_api_key:
        pytest.skip("groq_api_key not configured (.env missing GROQ_API_KEY)")
    cfg.llm_provider = "groq"
    cfg.fallback_chain = []
    sym, events_path = _build_symbion(cfg)
    try:
        yield sym, events_path
    finally:
        _cleanup_paths(cfg.db_path, events_path)


@pytest_asyncio.fixture
async def symbion_anthropic():
    """Symbion wired to Anthropic. Required for agent-loop tests because
    AnthropicClient is the only client with supports_tools=True."""
    cfg = _base_cfg()
    if not cfg.anthropic_api_key:
        pytest.skip("anthropic_api_key not configured (.env missing ANTHROPIC_API_KEY)")
    cfg.llm_provider = "anthropic"
    cfg.fallback_chain = []
    sym, events_path = _build_symbion(cfg)
    try:
        yield sym, events_path
    finally:
        _cleanup_paths(cfg.db_path, events_path)


@pytest_asyncio.fixture
async def symbion_anthropic_groq_fallback():
    """Anthropic primary with Groq fallback. Used by the breaker-trip test
    that asserts _active() routes to Groq when Anthropic's CB is open."""
    cfg = _base_cfg()
    if not cfg.anthropic_api_key:
        pytest.skip("anthropic_api_key not configured")
    if not cfg.groq_api_key:
        pytest.skip("groq_api_key not configured (needed as fallback target)")
    cfg.llm_provider = "anthropic"
    cfg.fallback_chain = ["groq"]
    sym, events_path = _build_symbion(cfg)
    try:
        yield sym, events_path
    finally:
        _cleanup_paths(cfg.db_path, events_path)


def read_turn_events(events_path: str) -> list:
    """Parse the temp event-log file and return only event=='turn' rows in
    write order. Tests typically run a single turn, so [-1] is the most
    recent. Returns [] when the file is empty / missing (test still gets
    a useful failure message rather than an IndexError)."""
    if not os.path.exists(events_path):
        return []
    rows = []
    with open(events_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            if entry.get("event") == "turn":
                rows.append(entry)
    return rows
