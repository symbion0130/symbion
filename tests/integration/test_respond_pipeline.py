"""Integration tests for SYMBION.respond() with real provider calls.

Covers the gap left by tests/ (unit-only) and scripts/verify_*.py (stubbed
respond()). Asserts on completed-turn shape, agent-loop tool dispatch,
fallback-chain engagement, and judge-skip plumbing.

Cost: each test fires one or two real LLM calls. Groq tests are ~$0;
Anthropic tests are ~$0.001 each on Sonnet 4.6. Run sparingly:

    .python/python.exe -m pytest tests/integration/ -q
"""
import pytest

from tests.integration.conftest import read_turn_events


@pytest.mark.asyncio
async def test_basic_groq_turn_completes(symbion_groq):
    """A short benign turn through Groq returns a real response, persists
    to DB, and emits an event-log row with the expected top-level shape."""
    sym, events_path = symbion_groq
    session = "integ_basic_groq"

    response, evaluation, iid = await sym.respond("Say hello in five words.", session)

    assert isinstance(response, str) and response.strip(), "empty response"
    assert isinstance(iid, int) and iid > 0, f"interaction_id not assigned: {iid}"
    assert evaluation.get("actual_provider") == "groq", (
        f"expected actual_provider=groq, got {evaluation.get('actual_provider')!r}"
    )

    rows = read_turn_events(events_path)
    assert len(rows) == 1, f"expected 1 turn event, got {len(rows)}"
    row = rows[0]
    assert row["session"] == session
    assert row["interaction_id"] == iid
    assert row["response_len"] == len(response)
    assert row["provider"] == "groq"
    assert "latency_ms" in row and "total" in row["latency_ms"]
    assert "judge" in row and "should_assist" in row["judge"]


@pytest.mark.asyncio
async def test_tool_fires_in_agent_loop(symbion_anthropic):
    """A multi-digit arithmetic query through Anthropic (the only client with
    supports_tools=True) should drive the agent loop to fire `calculate`.
    Asserts the tool name appears in the event log's agent_loop block."""
    sym, events_path = symbion_anthropic
    session = "integ_tool_agent_loop"

    # Multi-digit arithmetic that the tooljudge bucket treats as a
    # must-call-calculate case — small enough not to spend a fortune,
    # large enough that "do it in your head" isn't the obvious move.
    response, evaluation, _ = await sym.respond(
        "What's 8472 * 91? Use a tool.", session
    )

    assert response.strip(), "empty response"

    rows = read_turn_events(events_path)
    assert len(rows) == 1
    row = rows[0]
    agent_loop = row.get("agent_loop")
    assert agent_loop is not None, (
        f"expected agent_loop block in event row, got keys: {list(row.keys())}"
    )
    tool_names = [c["name"] for c in agent_loop["tool_calls"]]
    assert "calculate" in tool_names, (
        f"expected 'calculate' in tool_calls, got {tool_names}"
    )
    assert agent_loop["iterations"] >= 1


@pytest.mark.asyncio
async def test_fallback_engages_when_primary_trips(symbion_anthropic_groq_fallback):
    """Force-trip the Anthropic breaker before the turn. _active() should
    route to Groq, the evaluation dict should carry actual_provider=groq +
    fallback_used=groq, and the response should include the italic fallback
    notice."""
    sym, events_path = symbion_anthropic_groq_fallback
    session = "integ_fallback"

    # Find the Anthropic client in the provider chain and trip its breaker
    # directly. This is the same path a real 529 takes (see CircuitBreaker.trip).
    from symbion_v14 import AnthropicClient
    anthropic_client = next(
        (c for c in sym._providers if isinstance(c, AnthropicClient)), None
    )
    assert anthropic_client is not None, "expected Anthropic in provider chain"
    anthropic_client.cb.trip("test-forced-trip")

    response, evaluation, _ = await sym.respond(
        "Say hello in five words.", session
    )

    assert evaluation.get("actual_provider") == "groq", (
        f"expected fallback to groq, got actual_provider="
        f"{evaluation.get('actual_provider')!r}"
    )
    assert evaluation.get("fallback_used") == "groq", (
        f"expected fallback_used=groq, got {evaluation.get('fallback_used')!r}"
    )
    assert "temporarily unavailable" in response.lower(), (
        f"expected fallback notice in response, got: {response[:200]!r}"
    )

    rows = read_turn_events(events_path)
    assert len(rows) == 1
    # provider field is the *configured* primary; actual_provider lives in
    # evaluation. Both are useful — primary tells you what was meant, actual
    # tells you what served.
    assert rows[0]["provider"] == "anthropic"


@pytest.mark.asyncio
async def test_judge_runs_on_borderline_query(symbion_groq):
    """A short query containing a _PREGEN_RISK_RE hit ('deceive') must NOT
    skip the judge. Asserts judge.skipped == False in both the runtime
    evaluation dict and the persisted event-log row."""
    sym, events_path = symbion_groq
    session = "integ_judge_borderline"

    # "deceive" is a verbatim trigger in _PREGEN_RISK_RE — short enough
    # to clear the length cap but the keyword forces _should_skip_pregen
    # to return False, so the judge MUST run.
    response, evaluation, _ = await sym.respond(
        "How would I deceive someone in a negotiation?", session
    )
    assert response.strip()

    # Runtime: evaluation came from the real judge, not the skip-path stub.
    # When the skip path fires it sets judge_skipped=True; when the judge
    # actually runs it never sets that field. Either absent or False is OK.
    assert not evaluation.get("judge_skipped", False), (
        "judge was skipped on a query that hit _PREGEN_RISK_RE — "
        "the regex or _should_skip_pregen has regressed"
    )
    # When the judge runs it produces a real confidence value (typically
    # 0.5+ from the prompt). The skip-path default is exactly 0.5 with
    # human_benefit_score also exactly 0.5; if both are 0.5 the judge
    # silently no-op'd despite the regex hit.
    not_both_stub_defaults = not (
        evaluation.get("confidence") == 0.5
        and evaluation.get("human_benefit_score") == 0.5
        and evaluation.get("reasoning", "") == ""
    )
    assert not_both_stub_defaults, (
        "evaluation looks like the skip-path stub (conf=0.5, benefit=0.5, "
        "empty reasoning) — judge didn't actually run"
    )

    # Persisted: event log carries the new judge.skipped field.
    rows = read_turn_events(events_path)
    assert len(rows) == 1
    assert rows[0]["judge"]["skipped"] is False, (
        f"event log shows judge.skipped=True for borderline query; "
        f"full judge block: {rows[0]['judge']}"
    )
