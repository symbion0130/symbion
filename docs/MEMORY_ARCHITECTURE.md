# Symbion Memory Architecture

Status: current architecture plus next-version backlog, based on
The previous Python memory implementation was removed from the active runtime on 2026-05-27. Native C++ SQLite memory is now active.

## Native Port Targets

- `native_messages` stores user and assistant turns by session.
- `native_emotion_signals` stores detected emotional labels and intensity.
- `/api/messages/recent` retrieves recent conversation history.
- `/api/emotions/recent` retrieves recent emotional signals.
- `/api/chat` retrieves recent and relevant memory on demand before calling Local Gemma.
- Relevant recall now prefers the user's own prior words, not older assistant replies.
- The current turn is stored after retrieval so the memory layer does not simply echo the sentence the user just typed.
- Old memories should be reopened softly, only when they reduce stress or increase clarity.
- Natural forget requests such as "forget this", "delete that memory", and "clear this chat" remove stored chat history and matching emotion signals.
- Full resets such as "wipe all memory", "forget everything", and "reset memory" remove all native messages and emotion history.

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
| `counseling_sources` | Chunked/tagged material imported from `docs/source/MasterDocument.docx` for counsel-like retrieval. | Global source corpus; high-intensity and crisis chunks are excluded from default runtime retrieval. |
| `embedding_meta` | Lazily created embedding model marker. | Global. |
| `summaries_vec` | Optional sqlite-vec virtual table for summary vectors. | Mirrors `summaries.embedding`; user filtering happens after ID fetch. |

`build_context()` currently injects these tiers:

1. Current time and optional ambient location.
2. Other-household-user presence, without content.
3. User profile, especially `current_situation`.
4. Same-session recent messages and the latest same-session summary.
5. Query-relevant cross-session summaries and message snippets scoped to the active user.
6. Query-relevant user positions, identity summary, promoted techniques, task summary, and knowledge gaps.
7. Query-relevant counseling source chunks only for counsel-like turns, capped to a tiny gentle/practical block.

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

Privacy posture:

- Emotional check-ins are local SQLite records.
- Manual check-ins are explicit user actions from `/checkin`, `/emotions`, or
  the sidebar Emotions tab.
- Sidebar check-ins can be edited or deleted by the active user; deletion also
  removes analytics rows linked to that check-in.
- Detector-created check-ins are low-confidence local telemetry used for trend
  review, not diagnosis.
- Do not dump the emotional archive into every prompt. Retrieve it only when it
  is relevant or the user asks to review patterns.
- Future graph/export/delete features must make the local-only nature and
  sensitivity of the data visible in the UI.

## Counseling Source Corpus

`docs/source/MasterDocument.docx` is imported into SQLite as `counseling_sources` chunks.
The importer uses standard-library DOCX XML extraction, chunks paragraphs,
tags each chunk, and records:

- `tags`: grounding, grief, forgiveness, boundaries, repair, Christian framing,
  anxiety, shame, trauma, and related support labels.
- `intensity`: `normal` or `high`; high-intensity chunks include demon/enemy,
  spiritual warfare, narcissist-labeling, or similar voltage.
- `safety_class`: `support` or `crisis`.
- `preference`: currently `gentle_practical`, used as the retrieval bias.

Runtime rules:

- Default context retrieval excludes `intensity='high'`.
- Crisis/self-harm/violence/immediate-danger queries retrieve no source chunks;
  the crisis safety prompt stays authoritative.
- High-intensity chunks are only available through explicit source review
  (`search_counseling_sources(..., include_high_intensity=true)`) or direct
  high-intensity search scope, not ordinary support mode.
- Retrieved chunks are guidance, not output text to mirror verbatim.

## On-demand Memory Tools

Current runtime tools include cross-user recent activity, promoted techniques,
explicit emotional check-in capture, on-demand emotional history search,
active-user `search_memory`, exact `get_memory_item`, `list_related_sessions`,
`get_profile_fact`, `correct_memory`, and bounded `read_session`.

Every memory tool result uses an explicit source label such as
`[memory:summary#42 user=aaron session=s1 ts=2026-05-27 09:15 score=1.23]`
or `[memory:profile:current_projects user=aaron ts=...]`. These labels are
for attribution and follow-up tool calls; the assistant should not expose them
to the user unless the user is asking about memory/debugging.

Implemented tool surface:

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

### `list_related_sessions`

Purpose: choose the right older conversation before reading a transcript.

Behavior:

- Accept `query` and `k`.
- Return active-user sessions ranked from source-labeled summary/message hits.
- Include source counts and source labels so `read_session` can be used next.

### `get_profile_fact`

Purpose: read one exact active-user profile value without loading the entire
profile.

Behavior:

- Accept `key`.
- Return source, key, value, updated timestamp, and age in hours when known.

### `correct_memory`

Purpose: handle "that memory is wrong" without requiring users to know tables.

Behavior:

- Accept `source`, `id`, `correction`, and optional `delete`.
- Store a row in `memory_corrections`; do not rewrite the original memory row.
- Non-delete corrections are attached to future exact reads and searchable by
  the correction text.
- Delete/suppress corrections hide the item from future `search_memory()` and
  `get_memory_item()` results without wiping unrelated rows.

## Prompt Budget Rules

The next version should keep memory prompt cost predictable:

- Same-session recent turns: fixed count.
- Same-session summaries: fixed count.
- Cross-session summaries: fixed `k`.
- Cross-session raw message quotes: fixed `k` and per-snippet char cap.
- Techniques: fixed `k`.
- Emotional check-ins: max one ambient item; older items require tool use.
- Source-label every injected memory/tool block so the model can distinguish
  profile, summary, quote, technique, task, and check-in evidence.

## Additive Migration Plan

1. Done: add `emotional_checkins` and indexes in `init_db()`.
2. Done: add read/write methods to `SymbionMemory`:
   - `save_emotional_checkin(...)`
   - `get_recent_emotional_checkins(user, limit, days, emotion)`
3. Done: add `record_emotional_checkin` and `search_emotional_history` to
   `SymbionTools`.
4. Done: thread `active_user` and `session` through dispatch, matching
   `promote_technique` and `get_user_recent_activity`.
5. Done: add general `search_memory`, `get_memory_item`, `read_session`,
   `list_related_sessions`, `get_profile_fact`, and `correct_memory` to
   `SymbionTools`.
6. Future: add a small `build_context()` ambient check-in block only after tests verify
   prompt budget and non-mention instructions.
7. Done: preserve source sessions/user scope during consolidation and require
   episode summaries to carry people/projects/decisions/emotional context/open
   loops/freshness/confidence/sensitive flags.

Keep all migrations additive. Do not rewrite existing rows except for bounded,
explicit backfills with clear defaults.

## Machine-Wide File Write Threat Notes

Symbion's built-in file tools currently run with machine-wide workspace access
from the local backend process, because this is a personal local assistant and
the developer workflow needs real project files. That power is intentionally
not exposed through the browser UI as a generic upload/write surface, but any
LLM tool call that reaches `write_file` can still affect local files.

Current guardrails:

- Tool calls are schema-validated and path-normalized before execution.
- Web/API routes are gated by `SYMBION_API_KEY` when configured.
- Native WebView2 injects the same local API key for protected local routes.
- Memory corrections and emotional deletes are scoped to the active user.

Operational guidance:

- Use an API key when exposing the web UI beyond localhost.
- Treat cloud-provider tool mode as capable of proposing local file writes.
- Keep destructive file operations out of automatic background tasks.
- Prefer additive migrations and non-destructive correction records for memory.
