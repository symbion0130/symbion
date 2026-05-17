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
