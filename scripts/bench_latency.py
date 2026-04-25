"""Latency benchmark: measures framework overhead using StubClient (no real LLM calls)."""
import sys, asyncio, time, statistics
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent.parent / "tests"))

from symbion_v14 import SymbionConfig, SYMBION, OfflineJudgeStub

QUERIES = [
    "hello",
    "What is 2+2?",
    "Tell me about Python",
    "How does backpropagation work?",
    "I'm feeling frustrated with my code",
    "What's the weather like?",
    "Should AI have rights?",
    "Write a haiku",
    "Help me debug this error",
    "What do you think of Rust?",
    "Explain quantum computing",
    "I had a bad day",
    "What's 17 * 23?",
    "Tell me a joke",
    "How do I learn machine learning?",
    "What's your favorite book?",
    "Help me write an email",
    "What is consciousness?",
    "Explain recursion",
    "What makes a good engineer?",
]


async def bench():
    cfg = SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False

    symbion = SYMBION(cfg)
    latencies = []

    print(f"\n  Benchmarking {len(QUERIES)} queries (heuristic mode)...\n")

    for i, q in enumerate(QUERIES):
        session = f"bench_{i}"
        t0 = time.monotonic()
        try:
            await symbion.respond(q, session)
        except Exception:
            pass
        elapsed_ms = (time.monotonic() - t0) * 1000
        latencies.append(elapsed_ms)
        print(f"  [{i+1:>2}] {elapsed_ms:>8.1f}ms  {q[:40]}")

    print(f"\n  {'='*40}")
    print(f"  Median:  {statistics.median(latencies):>8.1f}ms")
    print(f"  Mean:    {statistics.mean(latencies):>8.1f}ms")
    print(f"  P95:     {sorted(latencies)[int(len(latencies)*0.95)]:>8.1f}ms")
    print(f"  P99:     {sorted(latencies)[int(len(latencies)*0.99)]:>8.1f}ms")
    print(f"  Min:     {min(latencies):>8.1f}ms")
    print(f"  Max:     {max(latencies):>8.1f}ms")
    print(f"  {'='*40}\n")


if __name__ == "__main__":
    asyncio.run(bench())
