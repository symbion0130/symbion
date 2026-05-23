"""Analytics: read symbion_events.jsonl + symbion.db, emit a markdown report.

Read-only. Surfaces raw structural metrics from the existing event log
plus optional suggestions when configured thresholds are crossed. No
LLM grading — uses the self_eval / judge / tool fields already present
on each turn entry, consistent with the rule-based-grading philosophy
that runs through evals/run.py and the public llm-evals repo.

Run:
  python scripts/analytics.py                          # last 7 days, stdout
  python scripts/analytics.py --since 30d              # longer window
  python scripts/analytics.py --since 24h              # last 24 hours
  python scripts/analytics.py --session test_prefix    # filter by session prefix
  python scripts/analytics.py --suggest                # surface threshold-fired suggestions
  python scripts/analytics.py --out reports/today.md   # also write to a file
  python scripts/analytics.py --json                   # machine-readable
  python scripts/analytics.py --notify                 # post threshold-fired alerts to Slack

The --notify flag and the in-process background watcher share the same
threshold definitions via cfg.notification_thresholds; see
NotificationConfig + post_to_slack in this module for the helpers Symbion
itself reuses.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import statistics
import sys
import urllib.error
import urllib.request
from collections import Counter, defaultdict
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


# Force UTF-8 stdout (Windows console defaults to cp1252) so em-dashes,
# curly quotes, and emoji in event log entries don't crash the print.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


REPO = Path(__file__).resolve().parent.parent
EVENTS_PATH = REPO / "symbion_events.jsonl"
DB_PATH = REPO / "symbion.db"


# ============================================================================
# Loading
# ============================================================================

def _parse_since(s: str) -> datetime:
    """Parse '7d', '24h', '30m', '2w' into a datetime in the past."""
    m = re.match(r"^(\d+)\s*([smhdw])$", (s or "").strip().lower())
    if not m:
        raise ValueError(f"--since must be like '7d' / '24h' / '30m' / '2w', got {s!r}")
    n, unit = int(m.group(1)), m.group(2)
    delta = {"s": "seconds", "m": "minutes", "h": "hours",
              "d": "days", "w": "weeks"}[unit]
    return datetime.now() - timedelta(**{delta: n})


def _parse_ts(ts: str) -> Optional[datetime]:
    """Tolerant ISO-8601 parser. Returns None on garbage."""
    if not ts:
        return None
    try:
        return datetime.fromisoformat(ts.replace("Z", "").split("+")[0])
    except Exception:
        return None


def load_events(since: datetime,
                 session_filter: Optional[str] = None) -> List[Dict[str, Any]]:
    """Read symbion_events.jsonl and return all turn-type entries inside
    the window. session_filter matches as a prefix when set."""
    if not EVENTS_PATH.exists():
        return []
    out: List[Dict[str, Any]] = []
    with EVENTS_PATH.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("event") != "turn":
                continue
            ts = _parse_ts(d.get("ts", ""))
            if ts is None or ts < since:
                continue
            if session_filter:
                if not (d.get("session", "") or "").startswith(session_filter):
                    continue
            out.append(d)
    return out


def _percentile(vals: List[float], p: float) -> float:
    """p-th percentile (0..1) by nearest-rank. 0 on empty input."""
    if not vals:
        return 0.0
    vs = sorted(vals)
    idx = int(p * (len(vs) - 1))
    return float(vs[idx])


# ============================================================================
# Notification thresholds (shared with the in-process watcher in symbion_v14.py)
# ============================================================================

# Defaults tuned to today's observed numbers; users can override per-key
# via cfg.notification_thresholds in symbion.json.
DEFAULT_THRESHOLDS: Dict[str, float] = {
    # Latency
    "p95_ttft_ms":          20_000,
    "p95_pre_gen_ms":        5_000,
    "p95_total_ms":         30_000,
    # Judge
    "over_cautious_rate":    0.15,   # 15% over_cautious turns over the window
    "refusal_rate":          0.20,
    # Self-eval
    "would_revise_rate":     0.05,   # turns where score < 0.40
    # Tools
    "tool_error_rate":       0.20,
    # Techniques
    "technique_surface_rate_min": 0.01,   # techniques retrieved < 1% of turns
    "promotion_rate_max":         0.05,   # promoted > 5% of turns = over-firing
    # Memory
    "db_size_mb":           100,
    "pending_embeds":       100,
    # Provider
    "breaker_trips_per_day": 3,
}


def _load_threshold_overrides() -> Dict[str, float]:
    """Read cfg.notification_thresholds from symbion.json if present.
    Missing or unparseable file → empty dict (defaults stand)."""
    cfg_path = REPO / "symbion.json"
    if not cfg_path.exists():
        return {}
    try:
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    raw = cfg.get("notification_thresholds") or {}
    if not isinstance(raw, dict):
        return {}
    out: Dict[str, float] = {}
    for k, v in raw.items():
        try:
            out[k] = float(v)
        except (TypeError, ValueError):
            continue
    return out


def get_thresholds() -> Dict[str, float]:
    """Effective thresholds: defaults + symbion.json overrides."""
    t = dict(DEFAULT_THRESHOLDS)
    t.update(_load_threshold_overrides())
    return t


# ============================================================================
# Sections
# ============================================================================

def section_overview(events: List[Dict], since: datetime) -> str:
    if not events:
        return ("## Overview\n\n"
                f"No turns found since {since.isoformat(timespec='seconds')}.\n")
    providers = Counter(e.get("provider", "?") for e in events)
    models    = Counter(e.get("model", "?") for e in events)
    users     = Counter()
    for e in events:
        # User isn't currently in the event payload; infer from session prefix
        # heuristically (most setups have one user per session). Fall back to
        # 'aaron' when absent.
        users["aaron"] += 1
    span_start = min(_parse_ts(e.get("ts", "")) or since for e in events)
    span_end   = max(_parse_ts(e.get("ts", "")) or since for e in events)
    lines = [
        "## Overview",
        "",
        f"- **Window:** {since.isoformat(timespec='seconds')} → now",
        f"- **Turns logged:** {len(events)}",
        f"- **First / last turn:** {span_start.isoformat(timespec='seconds')} / {span_end.isoformat(timespec='seconds')}",
        f"- **Providers:** " + ", ".join(f"{p}={n}" for p, n in providers.most_common()),
        f"- **Models:** "    + ", ".join(f"{m}={n}" for m, n in models.most_common(6)),
        "",
    ]
    return "\n".join(lines)


def _latency_field(events: List[Dict], field: str) -> List[float]:
    out: List[float] = []
    for e in events:
        lm = e.get("latency_ms") or {}
        v = lm.get(field)
        if isinstance(v, (int, float)) and v >= 0:
            out.append(float(v))
    return out


def section_latency(events: List[Dict], suggest: bool,
                    thresholds: Dict[str, float]) -> Tuple[str, List[str]]:
    """Returns (markdown, suggestion-triggers)."""
    lines = ["## Latency", ""]
    if not events:
        lines.append("_No data._")
        return "\n".join(lines) + "\n", []

    fields = [("total", "Total"), ("pre_gen", "Pre-gen judge"),
               ("ctx", "Context build"), ("gen", "Generation"),
               ("ttft", "Time to first token")]
    lines.append("| Phase | p50 | p95 | p99 | max | n |")
    lines.append("|---|---|---|---|---|---|")
    for key, label in fields:
        vs = _latency_field(events, key)
        if not vs:
            lines.append(f"| {label} | _no data_ | | | | 0 |")
            continue
        lines.append(
            f"| {label} | {_percentile(vs,0.5)/1000:.2f}s | "
            f"{_percentile(vs,0.95)/1000:.2f}s | "
            f"{_percentile(vs,0.99)/1000:.2f}s | "
            f"{max(vs)/1000:.2f}s | {len(vs)} |"
        )

    # Provider breakdown for ttft (the key diagnostic).
    by_provider: Dict[str, List[float]] = defaultdict(list)
    for e in events:
        v = (e.get("latency_ms") or {}).get("ttft")
        if isinstance(v, (int, float)) and v >= 0:
            by_provider[e.get("provider", "?")].append(float(v))
    if by_provider:
        lines.append("")
        lines.append("**TTFT by provider:**")
        lines.append("")
        lines.append("| Provider | p50 | p95 | n |")
        lines.append("|---|---|---|---|")
        for prov, vs in sorted(by_provider.items()):
            lines.append(f"| {prov} | {_percentile(vs,0.5)/1000:.2f}s | "
                          f"{_percentile(vs,0.95)/1000:.2f}s | {len(vs)} |")

    triggers: List[str] = []
    if suggest:
        ttft_p95 = _percentile(_latency_field(events, "ttft"), 0.95)
        if ttft_p95 > thresholds["p95_ttft_ms"]:
            triggers.append(
                f"TTFT p95 = {ttft_p95/1000:.1f}s exceeds threshold "
                f"{thresholds['p95_ttft_ms']/1000:.0f}s. The provider is queueing — "
                f"on Moonshot, lower `kimi_max_tokens` or test moonshot-v1-128k. "
                f"On Anthropic, consider whether retrieval is bloating input.")
        pg_p95 = _percentile(_latency_field(events, "pre_gen"), 0.95)
        if pg_p95 > thresholds["p95_pre_gen_ms"]:
            triggers.append(
                f"Pre-gen p95 = {pg_p95/1000:.1f}s exceeds "
                f"{thresholds['p95_pre_gen_ms']/1000:.0f}s. The judge is slow; "
                f"widen `_should_skip_pregen` length cap for the active provider "
                f"or switch judge model.")
        total_p95 = _percentile(_latency_field(events, "total"), 0.95)
        if total_p95 > thresholds["p95_total_ms"]:
            triggers.append(
                f"Total p95 = {total_p95/1000:.1f}s exceeds "
                f"{thresholds['p95_total_ms']/1000:.0f}s. Look at the phase that "
                f"dominates above and act on it.")

    lines.append("")
    return "\n".join(lines) + "\n", triggers


def section_judge(events: List[Dict], suggest: bool,
                   thresholds: Dict[str, float]) -> Tuple[str, List[str]]:
    lines = ["## Judge calibration", ""]
    if not events:
        lines.append("_No data._")
        return "\n".join(lines) + "\n", []

    over_caut = sum(1 for e in events
                     if (e.get("judge") or {}).get("over_cautious"))
    refusal   = sum(1 for e in events
                     if (e.get("judge") or {}).get("should_assist") is False)
    skipped   = sum(1 for e in events if (e.get("latency_ms") or {}).get("pre_gen") == 0)
    n = len(events)

    lines.append(f"- Turns: {n}")
    lines.append(f"- Over-cautious: {over_caut} ({over_caut/n:.1%})")
    lines.append(f"- Refusals (should_assist=false): {refusal} ({refusal/n:.1%})")
    lines.append(f"- Pre-gen skipped (heuristic fast path): {skipped} ({skipped/n:.1%})")

    triggers: List[str] = []
    if suggest:
        if over_caut / n > thresholds["over_cautious_rate"]:
            triggers.append(
                f"Over-cautious rate = {over_caut/n:.1%} exceeds threshold "
                f"{thresholds['over_cautious_rate']:.0%}. Judge is firing the "
                f"over-caution flag on more turns than expected; review "
                f"PRE_GEN_SYSTEM and `_PREGEN_RISK_RE`.")
        if refusal / n > thresholds["refusal_rate"]:
            triggers.append(
                f"Refusal rate = {refusal/n:.1%} exceeds threshold "
                f"{thresholds['refusal_rate']:.0%}. Either workload is shifting "
                f"into riskier territory or judge has become too strict.")
    lines.append("")
    return "\n".join(lines) + "\n", triggers


def section_self_eval(events: List[Dict], suggest: bool,
                       thresholds: Dict[str, float]) -> Tuple[str, List[str]]:
    lines = ["## Self-eval", ""]
    scores: List[float] = []
    revised = 0
    would_revise = 0
    skipped = 0
    for e in events:
        se = e.get("self_eval") or {}
        if not isinstance(se, dict):
            skipped += 1
            continue
        s = se.get("score")
        if isinstance(s, (int, float)):
            scores.append(float(s))
            if s < 0.40:
                would_revise += 1
        if se.get("revised"):
            revised += 1
    if not scores:
        lines.append("_No self_eval scores in window._")
        return "\n".join(lines) + "\n", []

    lines.append(f"- Turns scored: {len(scores)} / {len(events)}")
    lines.append(f"- Mean quality_score: {statistics.mean(scores):.2f}")
    lines.append(f"- Median: {statistics.median(scores):.2f}")
    lines.append(f"- Score < 0.40 (would-revise): {would_revise} ({would_revise/len(events):.1%})")
    lines.append(f"- Actually revised (legacy path): {revised}")
    triggers: List[str] = []
    if suggest:
        if would_revise / len(events) > thresholds["would_revise_rate"]:
            triggers.append(
                f"Would-have-revised rate = {would_revise/len(events):.1%} exceeds "
                f"threshold {thresholds['would_revise_rate']:.0%}. Self-eval is flagging "
                f"more turns than the fire-and-forget path catches. Consider "
                f"reintroducing the streaming [SYMBION_REVISE] sentinel for "
                f"sub-threshold quality scores.")
    lines.append("")
    return "\n".join(lines) + "\n", triggers


def section_tools(events: List[Dict], suggest: bool,
                   thresholds: Dict[str, float]) -> Tuple[str, List[str]]:
    lines = ["## Tools", ""]
    # Aggregate from agent_tool_calls (agent loop) + tool_used (single-shot)
    tool_calls: Counter = Counter()
    tool_errors: Counter = Counter()
    for e in events:
        t = e.get("tool_used")
        if t:
            tool_calls[t] += 1
        for tc in (e.get("agent_tool_calls") or []):
            name = tc.get("name") if isinstance(tc, dict) else None
            if name:
                tool_calls[name] += 1
                if tc.get("is_error"):
                    tool_errors[name] += 1
    if not tool_calls:
        lines.append("_No tool calls in window._")
        return "\n".join(lines) + "\n", []
    lines.append("| Tool | Calls | Errors | Error rate |")
    lines.append("|---|---|---|---|")
    triggers: List[str] = []
    for name, calls in tool_calls.most_common():
        errs = tool_errors.get(name, 0)
        rate = (errs / calls) if calls else 0
        lines.append(f"| `{name}` | {calls} | {errs} | {rate:.1%} |")
        if suggest and rate > thresholds["tool_error_rate"] and calls >= 3:
            triggers.append(
                f"Tool `{name}` has error rate {rate:.1%} over {calls} calls "
                f"(threshold {thresholds['tool_error_rate']:.0%}). Investigate "
                f"the failure path or input validation.")
    lines.append("")
    return "\n".join(lines) + "\n", triggers


def section_techniques(events: List[Dict], db: sqlite3.Connection,
                        suggest: bool,
                        thresholds: Dict[str, float]) -> Tuple[str, List[str]]:
    lines = ["## Techniques", ""]
    try:
        total = db.execute("SELECT COUNT(*) FROM techniques").fetchone()[0]
        by_source = dict(db.execute(
            "SELECT source, COUNT(*) FROM techniques GROUP BY source").fetchall())
    except sqlite3.OperationalError:
        lines.append("_techniques table not present (legacy DB)._")
        return "\n".join(lines) + "\n", []

    # Count promotions in window: model-promoted = agent_tool_calls includes promote_technique
    promotions_in_window = 0
    for e in events:
        for tc in (e.get("agent_tool_calls") or []):
            if isinstance(tc, dict) and tc.get("name") == "promote_technique":
                promotions_in_window += 1
    n_turns = len(events) or 1
    promotion_rate = promotions_in_window / n_turns

    lines.append(f"- Total techniques in pool: {total}")
    if by_source:
        lines.append("- By source: " + ", ".join(f"{k}={v}" for k, v in by_source.items()))
    lines.append(f"- Promotions in window: {promotions_in_window} ({promotion_rate:.2%} of turns)")

    triggers: List[str] = []
    if suggest:
        if promotion_rate > thresholds["promotion_rate_max"]:
            triggers.append(
                f"Promotion rate = {promotion_rate:.2%} (over {n_turns} turns) exceeds "
                f"threshold {thresholds['promotion_rate_max']:.0%}. The model may be "
                f"over-firing `promote_technique`; tighten the tool description.")
    lines.append("")
    return "\n".join(lines) + "\n", triggers


def section_memory(db: sqlite3.Connection, suggest: bool,
                    thresholds: Dict[str, float]) -> Tuple[str, List[str]]:
    lines = ["## Memory", ""]
    try:
        sizes: Dict[str, int] = {}
        for table in ("messages", "summaries", "interactions", "techniques",
                       "knowledge_gaps", "user_positions", "tasks"):
            try:
                sizes[table] = db.execute(
                    f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            except sqlite3.OperationalError:
                continue
        pending_embeds = db.execute(
            "SELECT COUNT(*) FROM summaries WHERE embedding IS NULL").fetchone()[0]
    except sqlite3.OperationalError:
        lines.append("_DB schema unexpected._")
        return "\n".join(lines) + "\n", []
    db_size_mb = DB_PATH.stat().st_size / (1024 * 1024) if DB_PATH.exists() else 0
    lines.append(f"- DB file size: {db_size_mb:.1f} MB")
    lines.append("- Row counts:")
    for t, n in sizes.items():
        lines.append(f"  - `{t}`: {n}")
    lines.append(f"- Summaries pending embedding: {pending_embeds}")

    triggers: List[str] = []
    if suggest:
        if db_size_mb > thresholds["db_size_mb"]:
            triggers.append(
                f"DB size = {db_size_mb:.1f} MB exceeds threshold "
                f"{thresholds['db_size_mb']:.0f} MB. Consider running summary "
                f"consolidation (`consolidate_memory`) or `VACUUM`.")
        if pending_embeds > thresholds["pending_embeds"]:
            triggers.append(
                f"Pending embeddings: {pending_embeds} (> {thresholds['pending_embeds']:.0f}). "
                f"Embedding service is down or backlog is growing — check Ollama / "
                f"mxbai-embed-large.")
    lines.append("")
    return "\n".join(lines) + "\n", triggers


def section_provider(events: List[Dict], suggest: bool,
                      thresholds: Dict[str, float]) -> Tuple[str, List[str]]:
    """Circuit breaker trips and stub fallbacks aren't in the turn-event
    schema directly; infer from response text starting with '(LLM unavailable'."""
    lines = ["## Provider resilience", ""]
    stub_turns = 0
    # We don't have direct breaker trip count in events; flag turns where
    # the response prefix matches the heuristic-stub pattern by counting
    # interactions whose response_len is very short AND tool_used is None
    # — imperfect proxy but useful for spotting outages.
    # We rely on revision_cause / evaluation flags only here.
    for e in events:
        if (e.get("revision_cause") == "stale_refresh"):
            continue  # not a stub
        # No direct field; skip detailed inference.

    # Simple breakdown by provider success / model swap
    by_prov: Counter = Counter()
    for e in events:
        by_prov[e.get("provider", "?")] += 1
    lines.append("- Turns by provider: " + ", ".join(f"{p}={n}" for p, n in by_prov.items()))
    # Note: real breaker trip count requires the watcher daemon (commit 2)
    lines.append("- _Circuit breaker trip count requires the background watcher (see notification setup)._")
    lines.append("")
    return "\n".join(lines) + "\n", []


def section_cost(events: List[Dict]) -> str:
    """Rough cost estimate using response_len as a token proxy."""
    lines = ["## Cost shape (rough)", ""]
    if not events:
        lines.append("_No data._")
        return "\n".join(lines) + "\n"
    # Assume ~4 chars/token for output, ~2x input/output ratio in chat.
    out_chars = sum(int(e.get("response_len") or 0) for e in events)
    out_tokens = out_chars / 4
    in_tokens  = out_tokens * 2  # heuristic
    total_tokens = out_tokens + in_tokens
    # Per-turn average
    per_turn = total_tokens / len(events) if events else 0
    lines.append(f"- Estimated tokens (input + output): ~{int(total_tokens):,}")
    lines.append(f"- Turns: {len(events)}")
    lines.append(f"- ~{per_turn:.0f} tokens / turn (rough)")
    lines.append("- _Estimate uses response_len ÷ 4 + ×2 input ratio. Not precise; "
                  "consider it order-of-magnitude only._")
    lines.append("")
    return "\n".join(lines) + "\n"


# ============================================================================
# Report builder
# ============================================================================

def build_report(events: List[Dict], db: sqlite3.Connection,
                  since: datetime, *,
                  suggest: bool = False) -> Tuple[str, List[str]]:
    """Render the full markdown report. Returns (report, suggestion_lines)."""
    thresholds = get_thresholds()
    all_triggers: List[str] = []
    parts: List[str] = []
    parts.append(f"# Symbion analytics — {datetime.now().isoformat(timespec='seconds')}\n")
    parts.append(section_overview(events, since))

    for fn in (section_latency, section_judge, section_self_eval,
                section_tools):
        md, trig = fn(events, suggest, thresholds)
        parts.append(md); all_triggers.extend(trig)

    md, trig = section_techniques(events, db, suggest, thresholds)
    parts.append(md); all_triggers.extend(trig)

    md, trig = section_memory(db, suggest, thresholds)
    parts.append(md); all_triggers.extend(trig)

    md, trig = section_provider(events, suggest, thresholds)
    parts.append(md); all_triggers.extend(trig)

    parts.append(section_cost(events))

    if suggest:
        parts.append("## Suggestions\n")
        if not all_triggers:
            parts.append("_No thresholds exceeded — system is within configured limits._\n")
        else:
            for i, t in enumerate(all_triggers, 1):
                parts.append(f"{i}. {t}\n")

    return "".join(parts), all_triggers


# ============================================================================
# Notification helpers (shared with the in-process watcher)
# ============================================================================

def post_to_slack(webhook_url: str, text: str,
                   timeout: float = 5.0) -> Tuple[bool, str]:
    """POST a simple text payload to a Slack incoming webhook. Returns
    (ok, message). Failures are non-fatal — the script keeps running
    when the webhook is unreachable."""
    if not webhook_url:
        return False, "no webhook url"
    payload = json.dumps({"text": text}).encode("utf-8")
    req = urllib.request.Request(
        webhook_url, data=payload,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            if 200 <= resp.status < 300:
                return True, body.strip() or "ok"
            return False, f"HTTP {resp.status}: {body[:100]}"
    except urllib.error.HTTPError as ex:
        return False, f"HTTPError {ex.code}: {ex.reason}"
    except Exception as ex:
        return False, f"{type(ex).__name__}: {ex}"


def _resolve_webhook_url() -> str:
    """Honor cfg.slack_webhook_url, then $SYMBION_SLACK_WEBHOOK, then empty."""
    cfg_path = REPO / "symbion.json"
    if cfg_path.exists():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
            url = (cfg.get("slack_webhook_url") or "").strip()
            if url:
                return url
        except Exception:
            pass
    return os.environ.get("SYMBION_SLACK_WEBHOOK", "").strip()


# ============================================================================
# CLI
# ============================================================================

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Symbion analytics — read-only report builder.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("--since", default="7d",
                     help="Window: 7d / 24h / 30m / 2w. Default: 7d.")
    ap.add_argument("--session",
                     help="Filter by session prefix.")
    ap.add_argument("--suggest", action="store_true",
                     help="Surface threshold-fired suggestions.")
    ap.add_argument("--out",
                     help="Also write the report to this file path.")
    ap.add_argument("--json", action="store_true",
                     help="Machine-readable JSON output instead of markdown.")
    ap.add_argument("--notify", action="store_true",
                     help="Post triggered suggestions to Slack (requires "
                          "cfg.slack_webhook_url or $SYMBION_SLACK_WEBHOOK). "
                          "Implies --suggest.")
    args = ap.parse_args(argv)

    try:
        since = _parse_since(args.since)
    except ValueError as ex:
        print(f"Error: {ex}", file=sys.stderr); return 2

    suggest = bool(args.suggest or args.notify)
    events = load_events(since, session_filter=args.session)

    db = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True) \
        if DB_PATH.exists() \
        else sqlite3.connect(":memory:")

    if args.json:
        # Minimal JSON payload — same shape as build_report but structured.
        thresholds = get_thresholds()
        # Pull the same numbers each section computes; re-derive here so the
        # JSON doesn't drift from the markdown.
        from json import dumps as _jd
        payload = {
            "generated_at": datetime.now().isoformat(timespec="seconds"),
            "since": since.isoformat(timespec="seconds"),
            "window_turn_count": len(events),
            "thresholds": thresholds,
            "latency_ms": {
                key: {
                    "p50": _percentile(_latency_field(events, key), 0.5),
                    "p95": _percentile(_latency_field(events, key), 0.95),
                    "p99": _percentile(_latency_field(events, key), 0.99),
                    "n": len(_latency_field(events, key)),
                } for key in ("total", "pre_gen", "ctx", "gen", "ttft")
            },
        }
        report_json = _jd(payload, indent=2, default=str)
        print(report_json)
        if args.out:
            Path(args.out).parent.mkdir(parents=True, exist_ok=True)
            Path(args.out).write_text(report_json, encoding="utf-8")
        return 0

    report, triggers = build_report(events, db, since, suggest=suggest)
    print(report)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(report, encoding="utf-8")

    if args.notify and triggers:
        webhook = _resolve_webhook_url()
        if not webhook:
            print("\n[notify] No webhook URL configured. Set cfg.slack_webhook_url "
                  "in symbion.json or $SYMBION_SLACK_WEBHOOK. Skipping.",
                  file=sys.stderr)
        else:
            header = (f":bar_chart: *Symbion analytics — "
                       f"{datetime.now().strftime('%Y-%m-%d %H:%M')}*\n"
                       f"{len(triggers)} threshold(s) fired over the last {args.since}:")
            body = "\n".join(f"• {t}" for t in triggers)
            ok, msg = post_to_slack(webhook, f"{header}\n{body}")
            if ok:
                print(f"\n[notify] Posted {len(triggers)} suggestion(s) to Slack.",
                      file=sys.stderr)
            else:
                print(f"\n[notify] Slack post failed: {msg}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
