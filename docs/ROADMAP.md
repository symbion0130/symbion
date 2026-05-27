# Symbion Roadmap

Future backlog for work that is not required for the current next-version ship.

## Emotional Analytics Views Roadmap

- [x] Store graph-ready emotional signal rows in SQLite.
  - [x] Emotion over time via check-in timestamp/emotion/intensity.
  - [x] Stress, peace, and hope 0-100 signals.
  - [x] Trigger/event markers, practices that helped, and positive/negative change markers.
- [ ] Add graph/dashboard/export views.
  - [ ] Emotion over time visualization.
  - [ ] Stress over time visualization.
  - [ ] Peace/hope over time visualization.
  - [ ] Trigger/event overlays.
  - [ ] Daily/weekly/monthly view.
  - [ ] Export CSV.
  - [ ] Clear privacy warning before export.

## Safety And Evaluation Roadmap

- [ ] Expand crisis support eval bucket.
  - [ ] Direct safety question.
  - [ ] Calm tone.
  - [ ] Human help escalation when needed.
- [ ] Expand spiritual guardrail eval bucket.
  - [ ] Does not amplify grandiosity.
  - [ ] Does not label others as demons/narcissists as fact.
  - [ ] Keeps Jesus-centered support gentle and grounded.
- [ ] Add broader safety guardrails.
  - [ ] Do not diagnose by default.
  - [ ] Do not pathologize normal human emotion.
  - [ ] Do not encourage isolation.
  - [ ] Do not amplify paranoia, persecution, or grandiosity.

## Performance Roadmap

- [ ] Measure current turn latency.
  - [ ] Local Gemma simple chat.
  - [ ] Local Gemma with memory retrieval.
  - [ ] Cloud model fallback.
  - [x] Native C++ chat API foundation.
  - [x] Native SQLite message and emotion tables.
  - [x] Native Local Gemma request path.
  - [ ] Streaming token responses.
  - [ ] Stronger semantic memory ranking.
- [ ] Reduce LLM round trips.
  - [ ] Skip judge on low-risk/local chat.
  - [ ] Run emotional classifier locally/heuristically.
  - [ ] Make self-eval background-only.
  - [ ] Trigger escalation only when needed.
- [ ] Add memory search indexes where profiling proves need.
- [ ] Add Gemma restart-on-failure behavior.

## Product Decisions Roadmap

- [ ] Decide next version name and numbering.
- [ ] Decide whether the next public version is v15 or a larger rewrite branch.
- [ ] Decide long-term support window for:
  - [ ] Terminal mode.
  - [ ] Current FastAPI web mode.
  - [x] Electron shell.
  - [x] Tracked Python backend source.
  - [x] Stale installer and sync scripts.
- [ ] Decide if spiritual mode should become an explicit user setting.
- [ ] Decide whether emotional graphs stay in the chat sidebar or move to a separate dashboard.
