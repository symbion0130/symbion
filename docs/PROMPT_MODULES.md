# Prompt Modules

Runtime prompt assembly lives in `symbion_v14.py`.

## Always-On Baseline

The static always-on baseline is the part measured by `always_on_prompt_line_counts()`:

| Module | Purpose | Agent lines |
| --- | --- | ---: |
| `SYMBION_PERSONA` | Core identity, style, honesty, and high-stakes rigor. | 23 |
| `CAPABILITIES_BASE` | Tool inventory and filesystem capability truth. | 18 |
| `CAPABILITIES_META` | Non-tool features: thinking trace, multi-user memory, judge/self-eval. | 7 |
| `CAPABILITIES_AGENT_MODE` | Native tool-use loop rules. | 10 |

Current static baseline: 61 physical lines in agent-loop mode, 52 in single-shot mode.
The prompt-slimming target is `PROMPT_LINE_BUDGET_TARGET = 200`.

These numbers intentionally exclude runtime-varying blocks: active-user attribution,
memory/profile preamble, mood, emotional-processing nudges, voice-loosen, tool data,
contradiction notices, and refusal instructions.

## Conditional Modules

- Active-user attribution is always added per turn, but its text varies by user.
- Memory/profile preamble is added only when `build_context()` returns content.
- Emotional processing mode is added only for distressed, grounding, or counsel-like turns.
- Emotional processing mode stays constrained to one simple follow-up question.
  When no intensity is present, it may make that one question a gentle optional
  intensity check; if the user skips rating/numbering, the prompt tells the
  model not to ask for it.
- Explicit work mode is added for concrete code, writing, editing, debugging,
  review, or structural file/path tasks. It lets the answer be as long and
  structured as the deliverable requires, and prevents emotional mode from
  replacing the requested work with a single follow-up question.
- Voice loosen is added only for short neutral/focused/excited casual turns.
- Tool data, contradiction notices, and over-caution instructions are turn-specific.

## Counseling Canon Boundary

`docs/COUNSELING_CANON.md` is the source-of-truth summary for counsel-like behavior.
It is not copied wholesale into the always-on prompt. The runtime prompt should keep
only the shortest emotional-mode rule inline, then retrieve or distill additional
counseling source material only when the user is actually asking for that kind of
support.
