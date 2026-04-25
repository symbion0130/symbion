"""Smoke test: instantiate Symbion with heuristic fallback, run one respond() call."""
import sys, asyncio
sys.path.insert(0, ".")

from symbion_v14 import SymbionConfig, SYMBION

def main():
    cfg = SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False

    symbion = SYMBION(cfg)
    response, evaluation, iid = asyncio.run(symbion.respond("hello", "smoke-session"))
    print(f"\nResponse: {response[:200]}")
    print(f"Evaluation: {evaluation}")
    print(f"Interaction ID: {iid}")
    print("\nSmoke test PASSED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
