"""Prompt-budget regression checks for the always-on module baseline."""

import sys

sys.path.insert(0, ".")

from symbion_v14 import (
    PROMPT_LINE_BUDGET_TARGET,
    always_on_prompt_line_count,
    always_on_prompt_line_counts,
    always_on_prompt_modules,
)


def test_always_on_prompt_line_counts_report_module_budget():
    counts = always_on_prompt_line_counts(agent_loop_active=True)

    assert counts["SYMBION_PERSONA"] > 0
    assert counts["CAPABILITIES_BASE"] > 0
    assert counts["CAPABILITIES_META"] > 0
    assert counts["CAPABILITIES_AGENT_MODE"] > 0
    assert counts["TOTAL"] == always_on_prompt_line_count(agent_loop_active=True)
    assert counts["TOTAL"] <= PROMPT_LINE_BUDGET_TARGET


def test_always_on_prompt_count_matches_joined_modules():
    modules = always_on_prompt_modules(agent_loop_active=True)
    joined = "\n\n".join(modules.values())

    assert always_on_prompt_line_count(agent_loop_active=True) == len(joined.splitlines())


def test_single_shot_prompt_baseline_is_smaller_than_agent_loop():
    agent_count = always_on_prompt_line_count(agent_loop_active=True)
    single_count = always_on_prompt_line_count(agent_loop_active=False)
    single_modules = always_on_prompt_modules(agent_loop_active=False)

    assert "CAPABILITIES_SINGLE_MODE" in single_modules
    assert single_count < agent_count
