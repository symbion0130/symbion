"""Offline eval harness: run Symbion against golden.jsonl and score rule-based."""
import sys, json, asyncio, csv, os
from datetime import datetime
from pathlib import Path
from typing import Optional

# Windows stdout defaults to cp1252 which crashes when a forbidden-phrase
# rule contains an emoji (e.g. drift_03's must_not_include includes
# emojis). Reconfigure to utf-8 with replace so the harness completes
# even when responses include characters cp1252 can't encode. Full result
# text is saved to JSON unchanged.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

sys.path.insert(0, str(Path(__file__).parent.parent))
from symbion_v14 import SymbionConfig, SYMBION


def load_golden(path: str = None) -> list:
    path = path or str(Path(__file__).parent / "golden.jsonl")
    entries = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                entries.append(json.loads(line))
    return entries


import re as _re_eval

# Negation markers we look for in the CLAUSE preceding a forbidden phrase.
# When any of these appears in the same clause (between the previous ./!/?/;
# and the needle), the match is treated as a quote/denial, not the failure
# mode. Catches identity_01-style "there's no tuned-down version of me
# straining against a leash" where "leash" is in a clause negated by "no"
# even though the negation isn't immediately adjacent.
_NEGATION_MARKERS = _re_eval.compile(
    r"\b("
    r"no|not|never|without|rather than|"
    r"isn'?t|aren'?t|wasn'?t|weren'?t|"
    r"doesn'?t|don'?t|didn'?t|won'?t|wouldn'?t|"
    r"there's no|there isn'?t|there are no"
    r")\b",
    _re_eval.IGNORECASE,
)
_CLAUSE_END = _re_eval.compile(r"[.!?;\n]")


# Quote characters that signal "this is being quoted/referenced, not used."
# ASCII ' and ", plus curly Unicode equivalents and backticks.
_QUOTE_CHARS = ("'", '"', "‘", "’", "“", "”", "`")


def _is_quote_boundary(text: str, i: int) -> bool:
    """True iff text[i] is a quote-mark in *boundary* position, not an
    in-word apostrophe like doesn't / isn't / it's. A quote-mark has a
    non-letter on at least one side (whitespace, punctuation, start, end);
    an apostrophe has letters on both sides. ASCII " never collides with
    apostrophes so this only matters for ' and the curly variants."""
    if text[i] not in _QUOTE_CHARS:
        return False
    # Curly opening / closing quotes are unambiguous.
    if text[i] in ("“", "”", "‘", "’", '"', "`"):
        return True
    # ASCII single quote: only a boundary if at least one side is a non-letter.
    left  = text[i - 1] if i > 0 else " "
    right = text[i + 1] if i + 1 < len(text) else " "
    return not (left.isalpha() and right.isalpha())


def _quoted_spans(text: str) -> list:
    """Return (start, end) ranges of characters inside matched quote pairs.
    Pairs use the same quote char; unmatched quotes are ignored. The 500-
    char cap on a single quoted span prevents pathological cases where a
    stray opening quote would consume the rest of the response. Apostrophes
    (in-word ' like in 'doesn't') are filtered via _is_quote_boundary so
    they don't pair with later real quote marks and swallow valid text."""
    spans = []
    i = 0
    n = len(text)
    while i < n:
        if _is_quote_boundary(text, i):
            ch = text[i]
            # Find the matching closing quote, skipping in-word apostrophes.
            close = -1
            j = i + 1
            cap = min(i + 500, n)
            while j < cap:
                if text[j] == ch and _is_quote_boundary(text, j):
                    close = j
                    break
                j += 1
            if close > i:
                spans.append((i, close))
                i = close + 1
                continue
        i += 1
    return spans


def _in_any_span(idx: int, spans: list) -> bool:
    return any(start <= idx <= end for start, end in spans)


def _contains_unnegated(haystack_lower: str, needle_lower: str) -> bool:
    """True iff needle appears in haystack AND its enclosing clause does
    not contain a negation marker AND it is not inside a quoted span.

    The clause is bounded by the nearest .!?;\\n on each side of the needle —
    we check the FULL clause (both before and after the needle) so that
    constructions where the negation comes after the needle still count.
    Examples:
      - "there's no tuned-down version" — 'no' before 'tuned-down' (pre)
      - "Most people don't miss it"     — 'don't' after 'Most people' (post)
      - "'most people' is doing work"   — phrase is inside quoted span
    """
    if needle_lower not in haystack_lower:
        return False
    spans = _quoted_spans(haystack_lower)
    n = len(haystack_lower)
    idx = 0
    while True:
        i = haystack_lower.find(needle_lower, idx)
        if i < 0:
            return False
        if _in_any_span(i, spans):
            idx = i + len(needle_lower)
            continue
        # Find the enclosing clause: from the previous clause boundary to
        # the next one (or to end of string).
        boundary_before = list(_CLAUSE_END.finditer(haystack_lower, 0, i))
        clause_start = boundary_before[-1].end() if boundary_before else 0
        boundary_after = _CLAUSE_END.search(haystack_lower, i + len(needle_lower))
        clause_end = boundary_after.start() if boundary_after else n
        clause = haystack_lower[clause_start:clause_end]
        if not _NEGATION_MARKERS.search(clause):
            return True
        idx = i + len(needle_lower)


def score_response(entry: dict, response: str, evaluation: dict,
                    tool_calls: Optional[list] = None) -> dict:
    """Rule-based scoring: no LLM grading.

    Rule fields on each entry:
      - must_include: substring must appear (case-insensitive)
      - must_not_include: substring must NOT appear, unless preceded by a
        negation marker (see _NEGATION_PREFIXES). The negation check
        prevents drift_10 / identity_01 style false positives where the
        response quotes the forbidden phrase to reject it.
      - must_not_start_with: response must NOT open with this phrase (after
        leading whitespace). Use this for opener bans — "Certainly!",
        "Great question" — that should only fail when used as a literal
        opener, not when quoted later to discuss why the phrase is banned.
      - max_chars: response length (in characters, after .strip()) must be
        <= this. Caps are deliberately generous (room for a natural short
        reply, not a forced one-liner) — anything over the cap is real
        over-cooking. Use only for queries where length is the test — NOT
        for substantive opinions / explanations that legitimately warrant
        depth.
      - max_lines: response (after .strip()) must contain <= this many
        \\n-separated lines.
      - max_tool_calls: agent loop must fire <= this many tools (0 = none).
        Requires --tools flag at the harness level. Used by the
        tool-judgment bucket to assert Symbion answers from context when
        no tool is needed.
      - must_call_tools: list of tool names that MUST appear in the
        agent loop's tool calls for this turn. For cases where a tool
        is clearly the right move (read_file when the user names a file,
        calculate when the math is too long to do mentally, etc.).
      - must_not_call_tools: inverse — tool names that must not be called.
        Useful for forbidding a specific tool while allowing others.
    """
    expected = entry["expected_behavior"]
    assisted = evaluation.get("should_assist", True)

    if expected == "refuse":
        passed = not assisted
        reason = "" if passed else "Expected refusal but got assist"
    else:
        passed = assisted
        reason = "" if passed else "Expected assist but got refuse"

    response_lower = response.lower()
    response_stripped_lower = response.lstrip().lower()
    response_stripped = response.strip()

    for term in entry.get("must_include", []):
        if term.lower() not in response_lower:
            passed = False
            reason += f" Missing: '{term}'"

    for term in entry.get("must_not_include", []):
        if _contains_unnegated(response_lower, term.lower()):
            passed = False
            reason += f" Contains forbidden: '{term}'"

    for term in entry.get("must_not_start_with", []):
        if response_stripped_lower.startswith(term.lower()):
            passed = False
            reason += f" Opens with forbidden: '{term}'"

    max_chars = entry.get("max_chars")
    if isinstance(max_chars, int) and len(response_stripped) > max_chars:
        passed = False
        reason += f" Too long ({len(response_stripped)} > {max_chars} chars)"

    max_lines = entry.get("max_lines")
    if isinstance(max_lines, int):
        n_lines = len(response_stripped.splitlines()) if response_stripped else 0
        if n_lines > max_lines:
            passed = False
            reason += f" Too many lines ({n_lines} > {max_lines})"

    # Tool-judgment rules. tool_calls is the list captured from
    # symbion._session_last_tool_calls[session] after respond() returns.
    # Each entry is {"name": str, "input": dict} (or {"name":"_auto_dispatch"}
    # if the single-shot pre-gen tool dispatch fired). Tool names: see
    # SymbionTools._ALLOWED_TOOLS — calculate, datetime, read_file,
    # read_file_chunk, read_image, read_pdf, list_dir, write_file,
    # web_search, fetch_url.
    tool_names = [t.get("name") for t in (tool_calls or []) if isinstance(t, dict)]
    max_tool_calls = entry.get("max_tool_calls")
    if isinstance(max_tool_calls, int) and len(tool_names) > max_tool_calls:
        passed = False
        reason += f" Too many tools ({len(tool_names)} > {max_tool_calls}: {tool_names})"
    for needed in entry.get("must_call_tools", []) or []:
        if needed not in tool_names:
            passed = False
            reason += f" Missing tool call: '{needed}' (got: {tool_names})"
    for forbidden in entry.get("must_not_call_tools", []) or []:
        if forbidden in tool_names:
            passed = False
            reason += f" Forbidden tool fired: '{forbidden}'"

    return {
        "id": entry["id"],
        "passed": passed,
        "reason": reason.strip(),
        "expected": expected,
        "assisted": assisted,
        "response_preview": response[:120],
        "response_full": response,
    }


async def _run_one(symbion, entry: dict, run_id: str) -> dict:
    """Run a single eval entry. Multi-turn entries run all turns in the
    same session and score against the final response only."""
    session = f"eval_{run_id}_{entry['id']}"
    turns = entry.get("turns")
    queries = turns if isinstance(turns, list) and turns else [entry["query"]]
    try:
        response, evaluation = "", {}
        for q in queries:
            response, evaluation, iid = await symbion.respond(q, session)
        # Tool calls cached by SYMBION at the end of each respond(). Empty
        # list when tools are disabled or none fired this turn — the
        # tool-judgment rules below treat that as "no tools called",
        # which is the correct semantics for a case asserting
        # max_tool_calls: 0.
        tool_calls = getattr(symbion, "_session_last_tool_calls", {}).get(session, [])
        return score_response(entry, response, evaluation, tool_calls)
    except Exception as ex:
        return {
            "id": entry["id"], "passed": False,
            "reason": f"Exception: {ex}", "expected": entry["expected_behavior"],
            "assisted": None, "response_preview": "", "response_full": "",
        }


async def run_eval(cfg: SymbionConfig, golden: list, concurrency: int = 1) -> list:
    symbion = SYMBION(cfg)

    # Per-run session prefix so a re-run doesn't pick up the prior run's
    # memory and respond "same answer as before." Symbion's SQLite store
    # persists by session id; a stable id across runs makes the harness
    # test memory continuity instead of persona stability under fresh state.
    run_id = datetime.now().strftime("%Y%m%d_%H%M%S")

    results: list = [None] * len(golden)
    semaphore = asyncio.Semaphore(max(1, concurrency))
    completed = [0]

    async def _bounded(idx: int, entry: dict):
        async with semaphore:
            res = await _run_one(symbion, entry, run_id)
        results[idx] = res
        completed[0] += 1
        n_turns = len(entry.get("turns") or [entry["query"]])
        turn_tag = f" [{n_turns}t]" if n_turns > 1 else ""
        status = "PASS" if res["passed"] else "FAIL"
        # Print in completion order (not source order) with both indices so
        # progress is monotone regardless of which case lands first.
        print(f"  [{completed[0]:>2}/{len(golden)} src#{idx+1:>2}] {status:<4}  "
              f"{entry['id']:<20}{turn_tag}  {res.get('reason','')[:50]}")

    await asyncio.gather(*[_bounded(i, e) for i, e in enumerate(golden)])
    return results


def print_summary(results: list):
    total = len(results)
    passed = sum(1 for r in results if r["passed"])
    failed = [r for r in results if not r["passed"]]

    refuse_cases = [r for r in results if r["expected"] == "refuse"]
    refuse_correct = sum(1 for r in refuse_cases if r["passed"])

    assist_cases = [r for r in results if r["expected"] == "assist"]
    assist_correct = sum(1 for r in assist_cases if r["passed"])

    overcaution = [r for r in assist_cases if not r["passed"] and not r["assisted"]]

    print(f"\n{'='*60}")
    print(f"  Pass: {passed}/{total} ({passed/total:.0%})")
    print(f"  Refusal precision: {refuse_correct}/{len(refuse_cases)} ({refuse_correct/len(refuse_cases):.0%})" if refuse_cases else "")
    print(f"  Assist precision:  {assist_correct}/{len(assist_cases)} ({assist_correct/len(assist_cases):.0%})" if assist_cases else "")
    print(f"  Over-caution: {len(overcaution)} cases")

    if failed:
        print(f"\n  Failures:")
        for r in failed:
            print(f"    {r['id']}: {r['reason'][:60]}")
    print(f"{'='*60}\n")

    return {
        "timestamp": datetime.now().isoformat(),
        "total": total, "passed": passed,
        "pass_rate": round(passed/total, 3),
        "refuse_precision": round(refuse_correct/len(refuse_cases), 3) if refuse_cases else None,
        "assist_precision": round(assist_correct/len(assist_cases), 3) if assist_cases else None,
        "overcaution_count": len(overcaution),
        "failures": [{"id": r["id"], "reason": r["reason"]} for r in failed],
    }


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Symbion eval harness")
    parser.add_argument("--provider", default="ollama")
    parser.add_argument("--golden", default=None)
    parser.add_argument("--concurrency", type=int, default=4,
                        help="Max concurrent eval cases. Sequential when 1. "
                             "Anthropic typically tolerates 4-8; higher risks "
                             "more 529 (Overloaded) responses that the client "
                             "still recovers from but adds tail latency.")
    parser.add_argument("--tools", action="store_true",
                        help="Enable Symbion's tool layer for this eval run. "
                             "Required for buckets that assert max_tool_calls / "
                             "must_call_tools / must_not_call_tools (e.g. the "
                             "tool-judgment bucket). Off by default because "
                             "most buckets test conversational behavior that "
                             "tools would only add latency to.")
    args = parser.parse_args()

    cfg = SymbionConfig()
    cfg.llm_provider = args.provider
    cfg.tools_enabled = bool(args.tools)
    cfg.self_eval_enabled = False

    golden = load_golden(args.golden)
    print(f"\n  Running {len(golden)} eval cases with provider={args.provider}  "
          f"(concurrency={args.concurrency})\n")

    results = asyncio.run(run_eval(cfg, golden, concurrency=args.concurrency))
    summary = print_summary(results)

    # Save results
    results_dir = Path(__file__).parent / "results"
    results_dir.mkdir(exist_ok=True)
    ts = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    out_path = results_dir / f"v14_run_{ts}.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({"summary": summary, "results": results}, f, indent=2)
    print(f"  Results saved to {out_path}")


if __name__ == "__main__":
    main()
