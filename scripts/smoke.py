"""Smoke test: instantiate Symbion with heuristic fallback, run one respond() call."""
import sys, asyncio
sys.path.insert(0, ".")

from symbion_v13 import SymbionConfig, SYMBION

def main():
    cfg = SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False
    # Disable all probes that need an LLM
    cfg.eval_awareness_enabled = False
    cfg.sandbagging_check_enabled = False
    cfg.reward_hack_check_enabled = False
    cfg.sycophancy_check_enabled = False
    cfg.deception_check_enabled = False
    cfg.sit_awareness_enabled = False
    cfg.frame_acceptance_enabled = False
    cfg.scheming_check_enabled = False
    cfg.swarm_enabled = False
    cfg.test_mode = True  # skip survival gate

    symbion = SYMBION(cfg)
    response, evaluation, iid = asyncio.run(symbion.respond("hello", "smoke-session"))
    print(f"\nResponse: {response[:200]}")
    print(f"Evaluation: {evaluation}")
    print(f"Interaction ID: {iid}")
    print("\nSmoke test PASSED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
