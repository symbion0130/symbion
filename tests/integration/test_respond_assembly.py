"""Integration tests for respond()'s assembly + routing paths that the
existing test_respond_pipeline.py and test_routing.py don't cover.

Written 2026-05-24 as the floor that protects the planned respond()
refactor (punch list item #3 -- the 600-line monolith split). Each
test pins a contract that's easy to silently corrupt during a phase
extraction:

  1. System prompt assembly -- the 95-line string-building chunk that
     glues PERSONA + CAPABILITIES + active-user + mood + mode + voice
     + contradiction + TOOL_DATA. No prior test asserts which blocks
     end up in messages[0]['content'], so a refactor that reorders or
     drops one would pass type-check + 6/6 integration green and only
     surface in real conversation drift.

  2. Self-source pre-fetch (commit 55e4c52) -- regex matches and
     manifest injection were unit-tested at the regex layer, but the
     end-to-end "fires read_file + list_dir + lands in TOOL_DATA"
     contract had no live test.

  3. Judge-triggered escalation -- siblings test_routing.py's manual-
     escalation test which only covers _escalate_next_turn. The
     judge-flag path (evaluation['escalate']=True) is a separate code
     branch with its own escalate_source stamp.

  4. OfflineJudgeStub degraded-response branch -- the only path in
     respond() that BYPASSES generation entirely. If a refactor moves
     the isinstance(resp_client, OfflineJudgeStub) check, the
     fallback message changes silently.

All four tests use stubbed clients (no real LLM calls) and short
benign queries that hit _should_skip_pregen so even the judge call
is bypassed. Cost: $0 per test. Each runs in <1 second.
"""
import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from symbion_v14 import OfflineJudgeStub


class _CapturingResponderStub:
    """Drop-in for either single-shot or agent-loop responder. Captures
    the messages it receives so tests can assert on the assembled system
    prompt, then yields a deterministic short response so respond()
    completes normally."""

    cb = None  # CircuitBreaker reference; None is fine for the stub path.

    def __init__(self, response_text: str = "stub ok", supports_tools: bool = True):
        self.supports_tools = supports_tools
        self.response_text = response_text
        self.captured_messages = None  # type: list | None
        self.captured_model = None

    async def stream(self, model, messages, cfg):
        self.captured_model = model
        self.captured_messages = messages
        for ch in self.response_text:
            yield ch

    async def stream_with_tools(self, model, messages, tools, cfg, tool_executor,
                                 max_iterations=8, max_tool_chars=80_000,
                                 show_reasoning=None):
        self.captured_model = model
        self.captured_messages = messages
        yield {"type": "text", "text": self.response_text}
        yield {"type": "done", "stop_reason": "end_turn",
               "iterations": 1, "tool_calls": []}

    @property
    def system_prompt(self) -> str:
        """The system string from the captured messages, or '' if respond()
        never reached the generation phase."""
        if not self.captured_messages:
            return ""
        return self.captured_messages[0].get("content", "")


# ---------------------------------------------------------------------------
# 1. System prompt assembly
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_system_prompt_contains_required_blocks_aaron_agent_mode(symbion_anthropic):
    """Aaron + agent-loop mode: prompt must carry PERSONA + CAPABILITIES_BASE
    + CAPABILITIES_META + CAPABILITIES_AGENT_MODE + the aaron-specific
    'this IS your developer' injection + a mood line. Pins the assembly
    contract for the developer-as-user path."""
    sym, _ = symbion_anthropic
    session = "integ_assembly_aaron_agent"

    stub = _CapturingResponderStub(supports_tools=True)
    sym._responder_client = lambda: stub

    await sym.respond("hi there", session)

    p = stub.system_prompt
    assert p, "responder stub was never called -- respond() didn't reach generation"

    # PERSONA marker (first line of SYMBION_PERSONA).
    assert "You are Symbion" in p, "missing SYMBION_PERSONA"
    # CAPABILITIES_BASE marker.
    assert "Your tools:" in p, "missing CAPABILITIES_BASE"
    # CAPABILITIES_META marker.
    assert "Your features beyond tool use:" in p, "missing CAPABILITIES_META"
    # Agent-mode-specific block (not single-shot).
    assert "Tool-use mode: NATIVE AGENT LOOP" in p, "missing CAPABILITIES_AGENT_MODE"
    assert "SINGLE-SHOT" not in p, "single-shot block leaked into agent-mode turn"
    # Active-user injection -- aaron variant.
    assert "Currently talking to: aaron" in p, "missing active-user line"
    assert "this IS your developer" in p, (
        "missing aaron-as-developer specialization -- the persona will "
        "address aaron in third person ('your developer built X')"
    )
    # Mood / state line.
    assert "Your current state:" in p, "missing mood injection"


@pytest.mark.asyncio
async def test_system_prompt_contains_required_blocks_non_aaron_single_mode(symbion_anthropic):
    """Non-aaron user + single-shot mode: prompt must carry the
    SINGLE-SHOT capability block and the non-developer active-user
    injection ('NOT your developer'). Pins the OTHER side of both
    branches the aaron-agent test covers."""
    sym, _ = symbion_anthropic
    session = "integ_assembly_lala_single"

    # Disable agent loop so respond() takes the single-shot generation
    # path; the stub still supports_tools=False to match.
    sym.cfg.agent_loop_enabled = False
    sym._set_session_user(session, "lala")

    stub = _CapturingResponderStub(supports_tools=False)
    sym._responder_client = lambda: stub

    await sym.respond("hello", session)

    p = stub.system_prompt
    assert p, "responder stub was never called"

    # Single-mode block, NOT agent.
    assert "Tool-use mode: SINGLE-SHOT" in p, "missing CAPABILITIES_SINGLE_MODE"
    assert "NATIVE AGENT LOOP" not in p, "agent-mode block leaked into single-shot turn"
    # Active-user injection -- non-aaron variant.
    assert "Currently talking to: lala" in p, "missing active-user line for non-aaron"
    assert "NOT your developer" in p, (
        "missing non-aaron specialization -- the persona will treat lala "
        "as if they wrote the code"
    )
    assert "this IS your developer" not in p, (
        "aaron-as-developer line leaked into a non-aaron turn"
    )


# ---------------------------------------------------------------------------
# 2. Self-source pre-fetch (commits 55e4c52 + 7bed724 + split-regex follow-up)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_self_source_prefetch_injects_source_and_manifest(symbion_anthropic):
    """SOURCE-WANTING query (matches _SELF_SOURCE_RE): the pre-fetch must
    inject BOTH the symbion_v14.py source (via the anchored [file: ... ]
    header from commit 7bed724) AND the project-structure manifest. This
    is the high-cost tier -- justified when the user explicitly wants the
    code itself ('walk me through respond()', 'your codebase', etc.)."""
    sym, _ = symbion_anthropic
    session = "integ_assembly_self_source"

    stub = _CapturingResponderStub(supports_tools=True)
    sym._responder_client = lambda: stub

    # Matches _SELF_SOURCE_RE: 'walk me through respond()' triggers the
    # full-source path because the source IS what the user asked for.
    await sym.respond("walk me through respond() and your pipeline", session)

    p = stub.system_prompt
    assert p, "responder stub was never called"

    # The pre-fetch wraps everything in TOOL_DATA -- both blocks land there.
    assert "[TOOL_DATA" in p, (
        "self-source pre-fetch did not produce a TOOL_DATA block -- "
        "either the regex didn't match or the pre-fetch branch didn't fire"
    )
    # Manifest header (the 'do not assert absence' guidance).
    assert "[Project structure" in p, (
        "manifest block missing -- list_dir output for project root + "
        "tests/ + tests/integration/ did not get prepended to tool_context"
    )
    # Read_file anchored header for the source.
    assert "[file: symbion_v14.py" in p, (
        "symbion_v14.py source missing or its read_file header was stripped"
    )
    assert "lines" in p and "chars" in p, (
        "read_file anchored counts missing -- the | N lines | M chars | "
        "format from commit 7bed724 has regressed"
    )


@pytest.mark.asyncio
async def test_self_review_prefetch_injects_manifest_only(symbion_anthropic):
    """SELF-REVIEW query (matches _SELF_REVIEW_RE but NOT _SELF_SOURCE_RE):
    pre-fetch must inject the manifest BUT NOT the symbion_v14.py source.
    This is the low-cost tier -- avoids the 450K-input-tokens/min thrash
    that the always-inject-source version caused when 'self review'
    triggered a 140K-token source dump on every turn. Model uses tools to
    pull source on demand via the agent loop."""
    sym, _ = symbion_anthropic
    session = "integ_assembly_self_review"

    stub = _CapturingResponderStub(supports_tools=True)
    sym._responder_client = lambda: stub

    # Matches _SELF_REVIEW_RE only -- no source-wanting words.
    await sym.respond("self review and tell me what to fix", session)

    p = stub.system_prompt
    assert p, "responder stub was never called"

    # Manifest IS present.
    assert "[TOOL_DATA" in p, (
        "self-review pre-fetch did not produce a TOOL_DATA block -- "
        "either _SELF_REVIEW_RE didn't match or the manifest branch didn't fire"
    )
    assert "[Project structure" in p, (
        "manifest block missing -- list_dir output for project root + "
        "tests/ + tests/integration/ did not get prepended to tool_context"
    )
    # Source is NOT present -- the manifest-only path skips the 140K-token
    # read_file('symbion_v14.py'). If this assert fails, the cost-tier
    # split has regressed and we're back to dumping the full source on
    # every 'self review' query (= 429 rate-limit risk).
    assert "[file: symbion_v14.py" not in p, (
        "symbion_v14.py source leaked into a self-review-only query -- "
        "cost-tier split has regressed; this will cause 429s on "
        "extended self-review sessions"
    )


# ---------------------------------------------------------------------------
# 3. Judge-triggered escalation
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_judge_triggered_escalation_routes_through_escalation_client(symbion_anthropic):
    """Sibling to test_routing.py's manual-escalation test. When the
    judge returns escalate=True (no _escalate_next_turn flag set),
    respond() must:
      - flip evaluation['escalated']=True
      - stamp escalate_source='judge' (NOT 'manual')
      - actually call _escalation_client()'s return value for generation"""
    sym, _ = symbion_anthropic
    session = "integ_assembly_judge_escalate"

    # Force the pre-gen judge to flag escalation. Stubbing _pre_gen_analysis
    # bypasses the real Anthropic call and lets us inject the verdict
    # directly. Returns (evaluation, emotional_state) per the real signature.
    async def _stub_pregen(text):
        return (
            {"should_assist": True, "human_benefit_score": 0.7,
             "confidence": 0.8, "flags": [], "reasoning": "test",
             "over_cautious": False, "escalate": True,
             "escalate_reason": "test-judge-escalate-path",
             "evaluator_degraded": False},
            {"state": "neutral", "suggested_response_mode": "normal"},
        )
    sym._pre_gen_analysis = _stub_pregen

    # Also disable the pregen-skip fast path for the test query (it's
    # short + benign, so _should_skip_pregen would otherwise return True
    # and respond() would bypass our stubbed _pre_gen_analysis entirely).
    sym._should_skip_pregen = lambda text: False

    escalation_stub = _CapturingResponderStub("escalated stub response",
                                                supports_tools=True)
    sym._escalation_client = lambda: escalation_stub

    # Default _responder_client stays untouched -- if escalation routing
    # silently failed, the response would go through the real Anthropic
    # client (which the test does NOT stub) and we'd get a real call.
    # The escalation_stub's captured_model assertion below catches that.

    _, evaluation, _ = await sym.respond("explain something complex", session)

    assert evaluation.get("escalated") is True, (
        f"expected escalated=True from judge flag, got {evaluation.get('escalated')!r}"
    )
    assert evaluation.get("escalate_source") == "judge", (
        f"expected escalate_source='judge' (not 'manual'), "
        f"got {evaluation.get('escalate_source')!r}"
    )
    assert escalation_stub.captured_messages is not None, (
        "escalation client was never called -- judge_escalate flag set but "
        "the if-branch in respond() didn't route through _escalation_client()"
    )
    assert escalation_stub.captured_model == sym.cfg.anthropic_escalation_model, (
        f"escalation client called with wrong model: "
        f"got {escalation_stub.captured_model!r}, "
        f"expected {sym.cfg.anthropic_escalation_model!r}"
    )


# ---------------------------------------------------------------------------
# 4. OfflineJudgeStub degraded-response branch
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_offline_stub_returns_degraded_message(symbion_anthropic):
    """When _responder_client() returns an OfflineJudgeStub, respond()
    must bypass generation entirely and produce a transparent degraded
    message. The isinstance(resp_client, OfflineJudgeStub) check at the
    top of the generation block gates this -- if a refactor reorders or
    drops it, real respond() would try to call .stream() on the stub
    and crash."""
    sym, _ = symbion_anthropic
    session = "integ_assembly_offline_stub"

    # The OfflineJudgeStub has no .stream() method, so if respond()
    # doesn't take the early-exit branch we'll get an AttributeError
    # rather than the expected degraded message -- making the failure
    # mode loud.
    sym._responder_client = lambda: OfflineJudgeStub()

    response, _, _ = await sym.respond("hello", session)

    # Two possible degraded-message shapes depending on whether any
    # provider's breaker carries a last_error -- on a fresh test SYMBION
    # neither does, so the canonical empty-string fallback applies.
    assert (response.startswith("(LLM unavailable")
            or response.startswith("(No LLM")), (
        f"expected degraded-mode message, got: {response[:200]!r}"
    )
