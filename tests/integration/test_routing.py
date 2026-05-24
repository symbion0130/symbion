"""Integration tests for the two routing paths Symbion's self-review
flagged as having no automated coverage: escalation client selection
and the stale-draft search-refresh pipeline.

Both tests use stubbed responder clients so they're deterministic and
don't burn API budget — but the rest of the pipeline (judge call,
context build, event log write, memory persistence) runs through the
real `respond()` so the test exercises actual integration, not just
the stubs.
"""
import asyncio
import sys
from pathlib import Path
from typing import AsyncIterator

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tests.integration.conftest import read_turn_events


class _StubAgentClient:
    """Minimal client stub that quacks like AnthropicClient enough for
    respond() to drive it through either single-shot or agent-loop path.
    stream() yields a fixed string; stream_with_tools yields a single
    text_delta then end_turn so the agent loop terminates immediately."""

    supports_tools = True
    cb = None

    def __init__(self, response_text: str = "stub response"):
        self.response_text = response_text
        self.stream_calls = []
        self.stream_with_tools_calls = []
        self.model_seen = None

    async def stream(self, model, messages, cfg):
        self.stream_calls.append(model)
        self.model_seen = model
        for ch in self.response_text:
            yield ch

    async def stream_with_tools(self, model, messages, tools, cfg, tool_executor,
                                 max_iterations=8, max_tool_chars=80_000,
                                 show_reasoning=None):
        # Signature matches AnthropicClient.stream_with_tools so respond()
        # can drive this stub through the agent-loop path without arg
        # arity errors. Yields one text event then terminates.
        self.stream_with_tools_calls.append(model)
        self.model_seen = model
        yield {"type": "text", "text": self.response_text}
        yield {"type": "done", "stop_reason": "end_turn",
               "iterations": 1, "tool_calls": []}


@pytest.mark.asyncio
async def test_manual_escalation_routes_through_escalation_client(symbion_anthropic):
    """`/escalate` (or programmatic _escalate_next_turn) flips a session
    into one-shot escalation mode. respond() must:
      - call _escalation_client() to get a stronger model
      - set evaluation['escalated'] = True
      - stamp escalated_to and escalate_source on evaluation
      - actually USE the returned client for generation (not the default
        responder)
    Test stubs _escalation_client so we don't pay Opus per run; the
    stub records that its stream() got called.
    """
    sym, events_path = symbion_anthropic
    session = "integ_escalation_manual"

    stub = _StubAgentClient("escalated stub response")
    sym._escalation_client = lambda: stub
    sym._escalate_next_turn[session] = True

    response, evaluation, _ = await sym.respond(
        "give me a deep technical breakdown", session
    )

    assert evaluation.get("escalated") is True, (
        f"expected escalated=True, got {evaluation.get('escalated')!r}"
    )
    assert evaluation.get("escalate_source") == "manual", (
        f"expected escalate_source=manual, got {evaluation.get('escalate_source')!r}"
    )
    assert evaluation.get("escalated_to") == sym.cfg.anthropic_escalation_model, (
        f"escalated_to should match anthropic_escalation_model, "
        f"got {evaluation.get('escalated_to')!r}"
    )
    # The stub must have actually been invoked — proves _escalation_client's
    # return value flowed all the way through to generation.
    assert (stub.stream_calls or stub.stream_with_tools_calls), (
        "escalation stub was never called — escalation routing wired "
        "evaluation['escalated']=True but didn't actually use the escalation client"
    )
    assert stub.model_seen == sym.cfg.anthropic_escalation_model, (
        f"escalation client was called with {stub.model_seen!r}, "
        f"expected {sym.cfg.anthropic_escalation_model!r} (the escalation model)"
    )


@pytest.mark.asyncio
async def test_stale_draft_triggers_search_refresh(symbion_anthropic):
    """When the responder returns text matching _STALE_RE ('as of my
    knowledge cutoff' etc), respond() should fire _search_and_inject
    and regenerate with the search result in the system prompt. Test
    constraints:
      - stale-draft path only runs in single-shot mode (not agent loop),
        so we disable agent_loop_enabled for this test
      - need tools_enabled=True for the path to even attempt
      - stub the responder client to return stale text first, fresh
        second; stub _search_and_inject to return non-empty so
        regeneration kicks in
      - assert event log carries stale_refresh=True and the second
        generation pass saw the injected search result
    """
    sym, events_path = symbion_anthropic
    session = "integ_stale_draft"

    sym.cfg.agent_loop_enabled = False
    sym.cfg.tools_enabled = True

    # Patch _search_and_inject so it returns a deterministic result
    # without touching Brave / DuckDuckGo (which would cost a real call
    # and depend on network).
    SEARCH_BLOCK = "LIVE: violet is the new amber as of 2026-05-24"
    sym._search_and_inject = lambda query: _instant(SEARCH_BLOCK)

    # Track which generation pass (first / second) the responder is on,
    # plus the messages each pass receives so we can verify the second
    # pass got the search-result-injected system prompt.
    call_log = {"count": 0, "messages_per_call": []}

    async def stale_then_fresh_stream(model, messages, cfg):
        call_log["count"] += 1
        call_log["messages_per_call"].append(messages)
        if call_log["count"] == 1:
            # Stale draft — contains a _STALE_RE keyword
            for ch in "as of my knowledge cutoff I don't have that data":
                yield ch
        else:
            # Fresh draft after search injection
            for ch in "violet is the answer; sourced from live search":
                yield ch

    # Patch the active client's stream method. The responder picked by
    # _responder_client() is the configured Anthropic client. We replace
    # ITS stream() so respond()'s code path is unaffected — only the
    # network call is mocked.
    sym._responder_client().stream = stale_then_fresh_stream

    # Query carefully avoids _SEARCH_TRIGGER_RE keywords ("right now",
    # "look up", "search", "latest", etc) — those would fire _maybe_tool
    # before generation, populate tool_context, and gate out the stale-
    # draft path entirely. Plain, non-time-anchored phrasing only.
    response, evaluation, _ = await sym.respond(
        "describe the watermark color scheme", session
    )

    assert call_log["count"] == 2, (
        f"expected two generation passes (stale + refresh), "
        f"got {call_log['count']}. _draft_is_stale or _search_and_inject "
        f"may not have triggered the refresh."
    )
    # The second pass's system prompt must contain the injected search block
    # so the model has the live data to work with.
    second_system = call_log["messages_per_call"][1][0]["content"]
    assert SEARCH_BLOCK in second_system, (
        "second generation pass did not receive the injected search block "
        "in its system prompt — the '--- LIVE WEB SEARCH RESULT ---' wrap "
        "may have changed shape and broken the refresh contract"
    )
    # Event log carries stale_refresh on the turn entry.
    rows = read_turn_events(events_path)
    assert len(rows) == 1
    assert rows[0]["stale_refresh"] is True, (
        f"event log should mark stale_refresh=True, got {rows[0]['stale_refresh']!r}"
    )
    # And the response should be the fresh second draft, not the stale first.
    assert "violet is the answer" in response, (
        f"expected fresh second-draft response in output, got: {response[:200]!r}"
    )


async def _instant(value):
    """Tiny helper to return an awaitable that resolves immediately to
    the given value. Used to stub _search_and_inject which is an async
    method but doesn't need to actually do anything async."""
    return value
