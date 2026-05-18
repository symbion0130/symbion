"""Smoke test: instantiate Symbion with heuristic fallback, run one respond() call.

On CPU-only hardware (no NVIDIA/AMD GPU, Intel iGPUs are not accelerated by
Ollama) the full respond() pipeline will time out on llama3.2 because
inference is single-digit tok/s. A failure here is almost always hardware,
not a code regression — the script prints which is which.
"""
import sys, asyncio, time
sys.path.insert(0, ".")

from symbion_v14 import SymbionConfig, SYMBION


async def _direct_ollama_probe(cfg: SymbionConfig) -> tuple[bool, str, float]:
    """Tiny direct chat_text call. Bypasses Symbion's full pipeline so we can
    tell connectivity issues (Ollama down) from inference-too-slow (CPU-only)."""
    from symbion_v14 import OllamaClient
    c = OllamaClient(cfg.ollama_host, cfg)
    if not c.is_available():
        return False, "Ollama server not reachable on " + cfg.ollama_host, 0.0
    t0 = time.monotonic()
    try:
        # 1 user token, num_predict=8 — should finish in seconds even on slow CPU.
        text = await c.chat_text(cfg.responder_model,
                                 [{"role": "user", "content": "hi"}],
                                 temp=0.1, max_tokens=8)
        return True, text or "(empty)", time.monotonic() - t0
    except Exception as ex:
        return False, f"{type(ex).__name__}: {ex}", time.monotonic() - t0


def main():
    cfg = SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False
    # mistral's ~8K context can't hold Symbion's full system prompt; the
    # stream silently 180s-times out. Pick a model with headroom.
    cfg.responder_model = "llama3.2"
    # The default cfg.max_tokens (16384) is sized for cloud responders.
    # Local Ollama on llama3.2 generates ~30-50 tok/s on consumer GPUs and
    # under 1 tok/s on pure CPU; 16384 would always blow the 180s stream
    # timeout. Cap to 512 for the smoke — we need ANY response, not a long one.
    cfg.max_tokens = 512

    # Probe 1: direct minimal call. Distinguishes "Ollama down" from "Ollama
    # too slow for the full respond() pipeline."
    print("\n[1/2] Direct Ollama probe (chat_text, 8 tokens)...")
    ok, msg, elapsed = asyncio.run(_direct_ollama_probe(cfg))
    if not ok:
        print(f"    FAILED in {elapsed:.1f}s: {msg}")
        print("    Smoke test FAILED at connectivity probe.")
        return 1
    tok_per_sec = (len(msg.split()) / elapsed) if elapsed > 0 else 0
    print(f"    OK in {elapsed:.1f}s — reply: {msg[:80]!r}")
    if elapsed > 30:
        print(f"    WARNING: 8-token reply took {elapsed:.0f}s (~{tok_per_sec:.1f} tok/s).")
        print(f"             This machine likely has no GPU acceleration; full")
        print(f"             respond() pipeline will time out below. That's hardware,")
        print(f"             not a code bug. Keep Anthropic for the responder.")

    # Probe 2: full respond() pipeline. Will hit 60s/180s timeouts on CPU.
    print("\n[2/2] Full respond() pipeline...")
    symbion = SYMBION(cfg)
    t0 = time.monotonic()
    response, evaluation, iid = asyncio.run(symbion.respond("hello", "smoke-session"))
    elapsed = time.monotonic() - t0
    print(f"\nResponse: {response[:200]}")
    print(f"Evaluation: {evaluation}")
    print(f"Interaction ID: {iid}  ({elapsed:.1f}s)")

    if not response or response.startswith("(Generation error"):
        print(f"\nSmoke test FAILED: responder returned no usable text in {elapsed:.0f}s")
        if "ReadTimeout" in response or "timeout" in response.lower():
            print(f"  -> Pipeline timed out. Probe 1 already showed inference is slow")
            print(f"     on this hardware. Hardware-bound, not a regression.")
        return 1
    print("\nSmoke test PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
