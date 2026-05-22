"""Multi-session stress probe — 5 parallel sessions, 3 turns each.

Sends 15 chat requests at /api/chat, scoring each for: HTTP status, latency,
whether the body looks like the heuristic stub (LLM unavailable) or a real
response, and any error text. Reports a summary table + per-session timeline.

Reads SYMBION_API_KEY from .env. Targets http://localhost:8000.
"""
from __future__ import annotations

import json
import os
import sys
import time
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ENV_PATH = REPO / ".env"
URL = "http://localhost:8000/api/chat"
N_SESSIONS = 5
TURNS_PER_SESSION = 3

# Varied, short prompts. Each session gets a thread of TURNS_PER_SESSION
# distinct messages so we exercise the session-sync + summarise paths.
PROMPTS = [
    ["hi", "what's 7 times 8?", "thanks, bye"],
    ["pick a color", "why that one", "name a song that matches"],
    ["one-word mood check", "what's behind that", "if you could undo one thing tonight, what"],
    ["tell me a fact about octopuses", "another one", "are they smarter than dogs"],
    ["status check — anything you'd flag?", "are tools up?", "anything in the gap tracker right now?"],
]


def read_api_key() -> str:
    for line in ENV_PATH.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("SYMBION_API_KEY"):
            _, _, val = line.partition("=")
            return val.strip().strip('"').strip("'")
    raise RuntimeError("SYMBION_API_KEY not in .env")


def looks_like_stub(text: str) -> bool:
    s = (text or "").lower()
    return any(needle in s for needle in [
        "llm unavailable",
        "overloaded",
        "circuit breaker",
        "no provider available",
    ])


def chat_once(api_key: str, session_id: str, message: str) -> dict:
    body = json.dumps({"message": message, "session_id": session_id}).encode("utf-8")
    req = urllib.request.Request(
        URL,
        data=body,
        method="POST",
        headers={"Content-Type": "application/json", "X-API-Key": api_key},
    )
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=90) as r:
            elapsed = time.perf_counter() - t0
            payload = json.loads(r.read().decode("utf-8", errors="replace"))
            resp = payload.get("response", "")
            return {
                "ok": True,
                "status": r.status,
                "elapsed": elapsed,
                "iid": payload.get("interaction_id"),
                "resp_len": len(resp),
                "resp_preview": resp[:90],
                "is_stub": looks_like_stub(resp),
            }
    except urllib.error.HTTPError as e:
        elapsed = time.perf_counter() - t0
        return {"ok": False, "status": e.code, "elapsed": elapsed, "err": str(e)}
    except Exception as e:
        elapsed = time.perf_counter() - t0
        return {"ok": False, "status": None, "elapsed": elapsed, "err": f"{type(e).__name__}: {e}"}


def run_session(api_key: str, sid_num: int, prompts: list[str]) -> list[dict]:
    session_id = f"stress_2026_05_22_s{sid_num}"
    results = []
    for turn_idx, msg in enumerate(prompts, start=1):
        wall = time.strftime("%H:%M:%S")
        print(f"  [{wall}] s{sid_num} t{turn_idx} -> {msg!r}", flush=True)
        r = chat_once(api_key, session_id, msg)
        r["session"] = sid_num
        r["turn"] = turn_idx
        r["msg"] = msg
        results.append(r)
        flag = ""
        if not r["ok"]:
            flag = f"  FAIL http={r['status']}: {r.get('err','')[:60]}"
        elif r.get("is_stub"):
            flag = "  STUB"
        print(f"  [{time.strftime('%H:%M:%S')}] s{sid_num} t{turn_idx} <- {r['elapsed']:.1f}s  status={r['status']}{flag}", flush=True)
    return results


def main() -> int:
    api_key = read_api_key()
    print(f"=== stress: {N_SESSIONS} sessions x {TURNS_PER_SESSION} turns = {N_SESSIONS*TURNS_PER_SESSION} requests ===", flush=True)
    t_start = time.perf_counter()
    all_results: list[dict] = []
    with ThreadPoolExecutor(max_workers=N_SESSIONS) as ex:
        futs = [ex.submit(run_session, api_key, i + 1, PROMPTS[i]) for i in range(N_SESSIONS)]
        for f in as_completed(futs):
            all_results.extend(f.result())
    t_total = time.perf_counter() - t_start

    # Summary
    total = len(all_results)
    success_real = sum(1 for r in all_results if r["ok"] and not r.get("is_stub"))
    stub = sum(1 for r in all_results if r["ok"] and r.get("is_stub"))
    fail = sum(1 for r in all_results if not r["ok"])
    latencies = [r["elapsed"] for r in all_results if r["ok"]]
    latencies.sort()
    p50 = latencies[len(latencies)//2] if latencies else 0.0
    p95 = latencies[int(len(latencies)*0.95)] if latencies else 0.0
    pmax = max(latencies) if latencies else 0.0

    print()
    print("=== summary ===")
    print(f"wall:           {t_total:.1f}s")
    print(f"total reqs:     {total}")
    print(f"  real resp:    {success_real}")
    print(f"  stub (529):   {stub}")
    print(f"  http fail:    {fail}")
    print(f"latency p50:    {p50:.1f}s")
    print(f"latency p95:    {p95:.1f}s")
    print(f"latency max:    {pmax:.1f}s")
    print()
    print("=== per-request detail ===")
    by_session = sorted(all_results, key=lambda r: (r["session"], r["turn"]))
    for r in by_session:
        s, t, e = r["session"], r["turn"], r["elapsed"]
        tag = "OK  " if r["ok"] and not r.get("is_stub") else ("STUB" if r.get("is_stub") else "FAIL")
        extra = ""
        if r["ok"]:
            extra = f"  iid={r.get('iid')}  resp={r.get('resp_preview','')!r}"
        else:
            extra = f"  err={r.get('err','')[:80]}"
        print(f"  s{s} t{t}  {tag}  {e:5.1f}s  status={r.get('status')}{extra}")
    return 0 if fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
