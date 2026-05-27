# Symbion Memory Architecture

Status: current architecture plus next-version backlog, based on
`symbion_v14.py` and `symbion_tools.py` as of 2026-05-26.

## Current SQLite Memory Surface

`init_db()` creates the core memory tables additively and migrates older
databases in place. Existing migrations add `summaries.embedding`,
`messages.user`, and `summaries.user`; profile scoping is encoded in
`user_profile.key` as `<user>:<key>` instead of a separate profile table.

Current memory-bearing tables:

| Table | Purpose | User/session behavior |
| --- | --- | --- |
| `messages` | Raw turns with `role`, `content`, `emotional_state`, `summarised`, and optional `user`. | Same-session reads are shared; cross-session retrieval is user-scoped. |
| `summaries` | Compressed long-term conversation memory with optional embedding blob. | User-scoped for cross-session retrieval. |
| `user_profile` | Profile facts, active session pointers, and location fields. | Bare keys are shared legacy base; `<user>:` keys override per user. |
| `interactions` | Evaluation/telemetry rows, including `emotional_state_detected`. | Session-scoped; not currently part of retrieval. |
| `self_model` | Formative events about Symbion itself. | Global. |
| `tasks` | Session task tracker with `next_checkin`. | Session-scoped. |
| `user_positions` / `contradictions` | Stated positions and detected conflicts. | Positions are session-tagged but not user-tagged today. |
| `knowledge_gaps` | Open knowledge gaps. | Session-scoped. |
| `proactive_queue` | Deferred proactive messages. | Session-scoped. |
| `learning_metrics` | Global adaptive metrics. | Global. |
| `techniques` | Promoted moves worth replicating, with optional embedding and sync source. | User-scoped retrieval. |
| `emotional_checkins` | First-class emotional events with emotion, intensity, valence, note, confidence, and capture source. | User-scoped and retrieved on demand. |
| `embedding_meta` | Lazily created embedding model marker. | Global. |
| `summaries_vec` | Optional sqlite-vec virtual table for summary vectors. | Mirrors `summaries.embedding`; user filtering happens after ID fetch. |

`build_context()` currently injects these tiers:

1. Current time and optional ambient location.
2. Other-household-user presence, without content.
3. User profile, especially `current_situation`.
4. Same-session recent messages and the latest same-session summary.
5. Query-relevant cross-session summaries and message snippets scoped to the active user.
6. Query-relevant user positions, identity summary, promoted techniques, task summary, and knowledge gaps.

The design already protects household-user confusion by keeping same-session
collaboration shared while scoping cross-session memory to the active user.

## Emotional Check-ins

Symbion now has a dedicated `emotional_checkins` table. Emotional signal still
also exists as inline turn metadata:

- `messages.emotional_state`
- `interactions.emotional_state_detected`
- profile fields such as `emotional_context` or `current_situation`

The check-in table preserves emotional events as first-class rows so future
graphs, reminders, trend review, and opt-in recall do not require stuffing
every prompt.

Implemented additive schema:

```sql
CREATE TABLE IF NOT EXISTS emotional_checkins (
    id INTEGER PRIMARY KEY,
    timestamp TEXT NOT NULL,
    session TEXT,
    user TEXT,
    emotion TEXT NOT NULL,
    intensity INTEGER,
    valence REAL,
    body_location TEXT,
    trigger TEXT,
    note TEXT,
    source_message_id INTEGER,
    confidence REAL,
    captured_by TEXT DEFAULT 'system'
);
```

Field notes:

- `captured_by`: `detector`, `tool`, `manual`, `scheduled`, or `imported`.
- `valence`: optional -1.0 to 1.0 mood direction.
- `intensity`: optional 0 to 100 strength.
- `note`: short source note, usually from the user turn.

Current retrieval behavior:

- Always user-scope check-ins.
- Do not inject the emotional archive into every prompt.
- Search check-ins through the on-demand `search_emotional_history` tool.
- Never use check-ins for cross-user retrieval unless the active user explicitly
  asks about another known user and the check-in visibility allows it.

## On-demand Memory Tools

Current runtime tools include cross-user recent activity, promoted techniques,
explicit emotional check-in capture, and on-demand emotional history search.
There is no general active-user `search_memory` tool yet, so deep recall is
still mostly limited to what `build_context()` preloads.

Recommended tool additions:

### `search_memory`

Purpose: active-user deep recall when the user asks about past conversations,
preferences, emotional history, decisions, or prior work and the answer is not
already present in context.

Suggested schema:

```json
{
  "name": "search_memory",
  "input_schema": {
    "type": "object",
    "properties": {
      "query": { "type": "string" },
      "scope": {
        "type": "string",
        "enum": ["all", "summaries", "messages", "techniques", "checkins", "profile"]
      },
      "k": { "type": "integer", "default": 6 }
    },
    "required": ["query"]
  }
}
```

Behavior:

- Search only the active user by default.
- Cap `query` length and `k`; default `k=6`, max `k=12`.
- Return source-labeled rows: table, id, timestamp, session, score, and a short
  preview.
- Prefer hybrid summary/technique retrieval where embeddings exist; use BM25 or
  substring fallback for tables without embeddings.
- Include `checkins` only when the query is about feelings, wellbeing, mood,
  stress, relationship continuity, or explicit "check-in" language.

### `get_memory_item`

Purpose: fetch one exact memory item after `search_memory` returns an ID.

Behavior:

- Accept `source` and `id`.
- Enforce active-user scope before returning content.
- Return full text for summaries/techniques/check-ins and a bounded window for
  messages.

### `correct_memory`

Purpose: handle "that memory is wrong" without requiring users to know tables.

Behavior:

- Accept `source`, `id`, `correction`, and optional `delete`.
- Mark or update the relevant row additively where possible.
- For summaries/messages that should not be edited destructively, add a
  correction record or profile override and exclude corrected rows from future
  retrieval if marked deleted.

## Prompt Budget Rules

The next version should keep memory prompt cost predictable:

- Same-session recent turns: fixed count.
- Same-session summaries: fixed count.
- Cross-session summaries: fixed `k`.
- Cross-session raw message quotes: fixed `k` and per-snippet char cap.
- Techniques: fixed `k`.
- Emotional check-ins: max one ambient item; older items require tool use.
- Source-label every injected memory block so the model can distinguish profile,
  summary, quote, technique, task, and check-in evidence.

## Additive Migration Plan

1. Done: add `emotional_checkins` and indexes in `init_db()`.
2. Done: add read/write methods to `SymbionMemory`:
   - `save_emotional_checkin(...)`
   - `get_recent_emotional_checkins(user, limit, days, emotion)`
3. Done: add `record_emotional_checkin` and `search_emotional_history` to
   `SymbionTools`.
4. Done: thread `active_user` and `session` through dispatch, matching
   `promote_technique` and `get_user_recent_activity`.
5. Future: add general `search_memory` and `get_memory_item` to `SymbionTools`.
6. Future: add a small `build_context()` ambient check-in block only after tests verify
   prompt budget and non-mention instructions.
7. Future: add `correct_memory` only after the search/fetch flow is stable.

Keep all migrations additive. Do not rewrite existing rows except for bounded,
explicit backfills with clear defaults.
