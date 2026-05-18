"""Smoke test: instantiate Symbion with heuristic fallback, run one respond() call."""
import sys, asyncio
sys.path.insert(0, ".")

from symbion_v14 import SymbionConfig, SYMBION

def main():
    cfg = SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False
    # mistral's ~8K context can't hold Symbion's full system prompt; the
    # stream silently 180s-times out. Pick a model with headroom.
    cfg.responder_model = "llama3.2"
    # The default cfg.max_tokens (16384) is sized for cloud responders.
    # Local Ollama on llama3.2 generates ~30-50 tok/s, so 16384 would need
    # 5+ minutes and blow the 180s stream timeout. Cap to 512 for the
    # smoke test — we just need ANY response, not a long one.
    cfg.max_tokens = 512

    symbion = SYMBION(cfg)
    response, evaluation, iid = asyncio.run(symbion.respond("hello", "smoke-session"))
    print(f"\nResponse: {response[:200]}")
    print(f"Evaluation: {evaluation}")
    print(f"Interaction ID: {iid}")

    if not response or response.startswith("(Generation error"):
        print(f"\nSmoke test FAILED: responder returned no usable text")
        return 1
    print("\nSmoke test PASSED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
