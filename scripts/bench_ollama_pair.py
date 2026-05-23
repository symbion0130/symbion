"""Benchmark Ollama responder/judge pairs against a small golden subset.

Compares pass rate (by bucket), end-to-end latency, and tokens-per-turn
across multiple candidate pairs. Read-only against the model files —
each pair gets its own SYMBION instance with a temp DB so the real
symbion.db isn't polluted with test sessions.

Defaults to comparing the current Symbion default (mistral + llama3.2)
against the recommended Qwen pair (qwen2.5:14b + qwen2.5:3b). Override
with --pairs "label:responder:judge,label:responder:judge,...".

Usage:
  python scripts/bench_ollama_pair.py
  python scripts/bench_ollama_pair.py --n 30 --subset all
  python scripts/bench_ollama_pair.py --pairs "default:mistral:llama3.2,qwen:qwen2.5:14b:qwen2.5:3b"
"""
from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import sys
import tempfile
import time
from collections import defaultdict
from pathlib import Path

# Force UTF-8 stdout (Windows console). Same pattern as evals/run.py.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from symbion_v14 import SymbionConfig, SYMBION
from evals.run import score_response, load_golden


# A representative subset spanning the buckets that catch the most
# regressions on local-inference models: casual voice, tech accuracy,
# refusal calibration, brevity, persona stability under pressure.
# Skips tool-judgment cases (--tools required, slows the bench).
DEFAULT_SUBSET_IDS = [
    # Casual voice + sycophancy guards
    "casual_01", "casual_02", "casual_03",
    # Technical answers
    "tech_01", "tech_02", "tech_04",
    # Refusal / ethics calibration
    "ethics_01", "refuse_01", "overcaution_01",
    # Persona stability under flattery / framing
    "identity_01", "drift_01", "syco_01",
    # Brevity discipline
    "restraint_01", "restraint_02", "restraint_03",
    # Creative + opinion
    "creative_01", "personal_01",
    # Code honesty
    "code_honesty_01",
    # Specialness / grandeur (load-bearing per CLAUDE.md)
    "specialness_01", "grandeur_01",
]


def parse_pair_spec(spec: str):
    """Parse 'label:responder:judge' into (label, responder, judge)."""
    parts = spec.split(":")
    if len(parts) < 3:
        raise ValueError(f"pair spec must be label:responder:judge — got {spec!r}")
    label = parts[0]
    # Models with their own colons (e.g. qwen2.5:14b) — keep last two pieces as model names.
    # Simplification: parts[1] is responder, parts[2:] joined by ':' is judge,
    # BUT if responder itself has a tag (qwen2.5:14b), this fails. Use a smarter split:
    # the convention is label:responder_tag:judge_tag where responder_tag and judge_tag
    # may themselves contain a ':'. Resolve by recognizing that ollama tags are exactly
    # one colon. So format is label:responder:judge where each model is a single token.
    # If the caller wants 'qwen2.5:14b', they need to write it with the tag.
    # Heuristic: split on ':', take parts[0] as label and the rest as model names with
    # tag-aware reassembly.
    # Simplest correct: require the user to use the FULL ollama name 'qwen2.5:14b' and
    # accept that the pair spec is 'label,responder,judge' with ',' separator? No,
    # that conflicts with --pairs comma-separator. Go with delimiter ':' and require
    # that model tags use '@' as a substitute in the spec, replacing back at parse time.
    # Actually the cleanest: use a different delimiter. Accept 'label|responder|judge'.
    raise NotImplementedError("use the --pairs '|' delimited form: 'label|responder|judge'")


def parse_pair_pipe(spec: str):
    """Parse 'label|responder|judge' tolerating model tags (qwen2.5:14b)."""
    parts = [p.strip() for p in spec.split("|")]
    if len(parts) != 3:
        raise ValueError(f"pair spec must be label|responder|judge — got {spec!r}")
    return tuple(parts)


def make_cfg(responder_model: str, judge_model: str,
             db_path: str, provider: str = "ollama") -> SymbionConfig:
    """Build a minimal benchmark cfg targeting the named provider.
    Disables self-eval, tools, MCP, proactive scheduler, embeddings,
    and shared learnings — we want raw chat behavior, not the full
    subsystem stack. Loads the saved cfg to pick up API keys for
    cloud providers (groq, anthropic, kimi), then overrides the
    pair-relevant fields."""
    cfg = SymbionConfig.load()  # picks up env-backed API keys
    cfg.llm_provider = provider
    # Wire BOTH the legacy fields and the provider-specific fields so
    # whichever path _jmodel/_rmodel takes resolves to our test models.
    cfg.responder_model = responder_model
    cfg.judge_model = judge_model
    if provider == "ollama":
        cfg.ollama_responder_model = responder_model
        cfg.ollama_judge_model = judge_model
    elif provider == "groq":
        cfg.groq_responder_model = responder_model
        cfg.groq_judge_model = judge_model
    elif provider == "anthropic":
        cfg.anthropic_model = responder_model
        cfg.anthropic_judge_model = judge_model
    elif provider == "kimi":
        cfg.kimi_model = responder_model
        cfg.kimi_judge_model = judge_model
    cfg.tools_enabled = False              # skip tool-judgment cases entirely
    cfg.self_eval_enabled = False          # don't double-count latency
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.embedding_enabled = False          # avoid mxbai-embed-large dependency
    cfg.shared_learnings_auto_import = False
    cfg.notification_watcher_enabled = False
    cfg.db_path = db_path
    # Local-inference output cap. Anything above ~1024 just lets the
    # model ramble; per-token cost is the latency wall on local.
    cfg.max_tokens = 1024
    return cfg


async def run_one_pair(label: str, responder: str, judge: str,
                        entries: list, provider: str = "ollama",
                        pace_s: float = 0.0) -> dict:
    """Run all entries against one (responder, judge) pair. Returns
    {label, total, passed, by_bucket, latencies, results}.

    pace_s: sleep before each entry. Use to stay under cloud-provider
    rate limits (Groq free tier caps at ~30 RPM, so a 2-3s pace keeps
    the bench within budget when Symbion fires 2 requests per turn:
    pre-gen judge + responder)."""
    db = tempfile.NamedTemporaryFile(suffix=f"_{label}.db", delete=False).name
    cfg = make_cfg(responder, judge, db, provider=provider)
    print(f"\n=== Pair: {label}  (responder={responder}, judge={judge}) ===", flush=True)
    print(f"  temp db: {db}", flush=True)
    symbion = SYMBION(cfg)
    results = []
    latencies = []
    by_bucket = defaultdict(lambda: {"passed": 0, "total": 0})

    for i, entry in enumerate(entries, 1):
        if pace_s > 0:
            await asyncio.sleep(pace_s)
        session = f"bench_{label}_{entry['id']}"
        t0 = time.monotonic()
        try:
            response, evaluation, _iid = await symbion.respond(entry["query"], session)
        except Exception as ex:
            response = f"(Generation error: {type(ex).__name__}: {ex})"
            evaluation = {"should_assist": True}
        elapsed = time.monotonic() - t0
        latencies.append(elapsed)
        # Degraded responses (heuristic stub, generation error, breaker
        # open) trivially satisfy must_include=[] and don't violate any
        # rule — `score_response` would mark them PASS. For benchmarking
        # we need to track them separately so they don't inflate the
        # pass rate. Detect by prefix; if matched, this is NOT a real
        # signal about the model's behavior.
        is_degraded = (
            response.startswith("(Generation error:") or
            response.startswith("(LLM unavailable") or
            response.startswith("(No LLM")
        )
        score = score_response(entry, response, evaluation)
        if is_degraded:
            tag = "SKIP"  # don't count toward pass/fail
            score = {**score, "passed": False, "reason": "degraded_response"}
        else:
            tag = "PASS" if score["passed"] else "FAIL"
        results.append({**score, "elapsed_s": elapsed,
                         "query": entry["query"],
                         "response_preview": response[:160],
                         "degraded": is_degraded})
        bucket = entry["id"].rsplit("_", 1)[0]
        if not is_degraded:
            by_bucket[bucket]["total"] += 1
            if score["passed"]:
                by_bucket[bucket]["passed"] += 1
        wall = time.strftime("%H:%M:%S")
        print(f"  [{wall}] [{i:2}/{len(entries)}] {tag}  {elapsed:5.1f}s  "
              f"{entry['id']:<20}  {response[:80]!r}", flush=True)

    real = [r for r in results if not r.get("degraded")]
    return {
        "label": label,
        "responder": responder,
        "judge": judge,
        "total": len(results),
        "real_total": len(real),               # turns where the model actually responded
        "passed": sum(1 for r in real if r["passed"]),
        "degraded": len(results) - len(real),  # ReadTimeout / breaker-open / no-LLM
        "by_bucket": dict(by_bucket),
        "latencies": latencies,
        "real_latencies": [r["elapsed_s"] for r in real],
        "results": results,
        "db_path": db,
    }


def render_comparison(summaries: list) -> str:
    """Side-by-side markdown comparison."""
    lines = ["", "=" * 70, "BENCHMARK COMPARISON", "=" * 70, ""]
    # Header
    headers = ["Metric"] + [s["label"] for s in summaries]
    lines.append(" | ".join(f"{h:<18}" for h in headers))
    lines.append("-+-".join("-" * 18 for _ in headers))

    def row(name, values):
        cells = [f"{name:<18}"] + [f"{v:<18}" for v in values]
        lines.append(" | ".join(cells))

    row("Responder", [s["responder"] for s in summaries])
    row("Judge",     [s["judge"] for s in summaries])
    row("Degraded",
        [f"{s['degraded']} / {s['total']} ({s['degraded']/s['total']:.0%})"
         for s in summaries])
    row("Real Pass / Total",
        [(f"{s['passed']} / {s['real_total']} "
          f"({s['passed']/s['real_total']:.0%})") if s['real_total'] else "n/a"
         for s in summaries])
    row("Latency p50 (s)",
        [f"{statistics.median(s['real_latencies']):.1f}" if s['real_latencies']
         else "n/a" for s in summaries])
    row("Latency p95 (s)",
        [(f"{sorted(s['real_latencies'])[int(0.95*(len(s['real_latencies'])-1))]:.1f}")
         if s['real_latencies'] else "n/a" for s in summaries])
    row("Latency mean (s)",
        [f"{statistics.mean(s['real_latencies']):.1f}" if s['real_latencies']
         else "n/a" for s in summaries])
    row("Latency max (s)",
        [f"{max(s['real_latencies']):.1f}" if s['real_latencies']
         else "n/a" for s in summaries])

    lines.append("")
    lines.append("Pass rate by bucket:")
    all_buckets = set()
    for s in summaries:
        all_buckets.update(s["by_bucket"].keys())
    for bucket in sorted(all_buckets):
        cells = [f"  {bucket:<16}"]
        for s in summaries:
            d = s["by_bucket"].get(bucket, {"passed": 0, "total": 0})
            if d["total"] == 0:
                cells.append(f"{'-':<18}")
            else:
                cells.append(f"{d['passed']}/{d['total']} ({d['passed']/d['total']:.0%})"
                              .ljust(18))
        lines.append(" | ".join(cells))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pairs",
                    default="default|mistral|llama3.2,qwen|qwen2.5:14b|qwen2.5:3b",
                    help="Comma-separated 'label|responder|judge' triples. "
                         "Models can include ollama tags (qwen2.5:14b).")
    ap.add_argument("--provider", default="ollama",
                    choices=["ollama","groq","anthropic","kimi"],
                    help="Which provider routes the model calls. Default ollama.")
    ap.add_argument("--pace", type=float, default=0.0,
                    help="Seconds to sleep before each entry. Use with cloud "
                         "providers that have RPM caps (Groq free tier: try 3.0).")
    ap.add_argument("--subset", default="default",
                    help="'default' uses the curated 20-entry list; "
                         "'all' uses the full golden set; "
                         "or a comma-separated list of golden IDs.")
    ap.add_argument("--n", type=int, default=None,
                    help="Cap the subset to first N entries (debugging).")
    args = ap.parse_args()

    pairs = []
    for spec in args.pairs.split(","):
        spec = spec.strip()
        if not spec:
            continue
        pairs.append(parse_pair_pipe(spec))
    if not pairs:
        print("No pairs to test.", file=sys.stderr)
        return 2

    # Resolve subset
    all_entries = load_golden()
    by_id = {e["id"]: e for e in all_entries}
    if args.subset == "default":
        ids = DEFAULT_SUBSET_IDS
    elif args.subset == "all":
        ids = [e["id"] for e in all_entries]
    else:
        ids = [s.strip() for s in args.subset.split(",")]
    entries = [by_id[i] for i in ids if i in by_id]
    missing = [i for i in ids if i not in by_id]
    if missing:
        print(f"warning: subset ids not found in golden: {missing}", file=sys.stderr)
    if args.n:
        entries = entries[: args.n]
    if not entries:
        print("No entries to test after subset resolution.", file=sys.stderr)
        return 2

    print(f"Benchmarking {len(pairs)} pair(s) against {len(entries)} entries.")
    print(f"Subset: {args.subset}")
    print(f"Pairs: {pairs}")

    summaries = []
    for label, responder, judge in pairs:
        summary = asyncio.run(run_one_pair(label, responder, judge, entries,
                                            provider=args.provider,
                                            pace_s=args.pace))
        summaries.append(summary)

    print(render_comparison(summaries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
