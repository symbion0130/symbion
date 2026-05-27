# Memory Test Plan

Status: proposal for the next-version memory work. These are tests to add when
the `emotional_checkins` table and on-demand memory tools are implemented.

## Schema and Migration

- `init_db()` creates `emotional_checkins` on a fresh database.
- `init_db()` upgrades an existing database without dropping existing
  `messages`, `summaries`, `user_profile`, or `techniques` rows.
- `emotional_checkins` defaults are stable:
  - `user` defaults to `aaron`
  - `source` defaults to `explicit`
  - `visibility` defaults to `ambient`
- Indexes exist:
  - `idx_checkins_user_time`
  - `idx_checkins_followup`

## Emotional Check-ins

- `save_emotional_checkin()` persists user, session, mood label, valence,
  intensity, note, assistant response, follow-up time, and visibility.
- `get_recent_emotional_checkin()` returns only the active user's eligible
  recent check-in.
- Resolved check-ins do not appear as ambient context.
- Stale check-ins include an age note if surfaced.
- `visibility='on_demand'` is excluded from ambient `build_context()`.
- `visibility='private'` is excluded from ambient context and cross-user tools.

## Build Context Budget

- Ambient check-ins add at most one block to the system prompt.
- The check-in block contains "do not mention unprompted" style guidance.
- Existing memory blocks still appear in the expected order:
  current time, location if present, cross-user presence, profile, session
  summary, relevant summaries, quotes, positions, identity, techniques, tasks,
  gaps.
- Cross-session retrieval remains scoped to the active user.
- Same-session recent messages remain shared and attributed when multiple users
  write to the same session.

## `search_memory`

- Rejects unknown scopes.
- Caps `k` to the configured maximum.
- Searches the active user's summaries without returning another user's rows.
- Searches messages with session, timestamp, role, ID, and preview labels.
- Searches techniques and includes `source` labels.
- Searches check-ins only when `scope='checkins'` or `scope='all'` with an
  emotionally relevant query.
- Returns source-labeled rows with table, id, timestamp, session, score, and
  preview.
- Falls back gracefully when embeddings or sqlite-vec are unavailable.

## `get_memory_item`

- Fetches a summary by ID only when it belongs to the active user.
- Fetches a bounded message window around a message ID.
- Fetches a technique by ID only when it belongs to the active user.
- Fetches a check-in by ID only when it belongs to the active user and is not
  private.
- Returns a clear not-found or not-authorized result without leaking whether
  another user's private row exists.

## `correct_memory`

- Records a correction for a summary or message without destructive deletion by
  default.
- Supports explicit delete/suppress behavior only after confirmation.
- Excludes suppressed memory rows from future `search_memory()` results.
- Leaves an audit trail with timestamp, active user, source table, source ID,
  and correction text.

## Tool Dispatch

- `search_memory`, `get_memory_item`, and `correct_memory` validate argument
  types and length caps in `SymbionTools._validate_args()`.
- Dispatch passes both `active_user` and `session` into every memory tool.
- Calling an active-user memory tool without `active_user` fails closed.
- `get_user_recent_activity` remains cross-user-only and still rejects self
  queries.

## Regression Coverage

- Existing retrieval tests still pass with check-ins absent from normal summary
  retrieval.
- Existing tool tests still pass when `SymbionTools` is constructed without a
  memory object; memory tools return an explicit wiring error instead of
  raising.
- Existing integration tests still pass with `tools_enabled=False`.
