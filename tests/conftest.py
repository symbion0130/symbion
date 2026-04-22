"""Test fixtures: StubClient that implements BaseClient's interface with pre-programmed responses."""
import sys
sys.path.insert(0, ".")

import pytest
from typing import AsyncIterator, Dict, List
from symbion_v13 import BaseClient, SymbionConfig, SYMBION


class StubClient(BaseClient):
    """A test double for BaseClient that returns canned responses."""

    def __init__(self, responses: Dict[str, str] = None, default_response: str = "Stub response."):
        super().__init__()
        self._responses = responses or {}
        self._default = default_response
        self._call_log: List[Dict] = []

    def _pick(self, user_text: str) -> str:
        for key, val in self._responses.items():
            if key in user_text:
                return val
        return self._default

    async def chat_json(self, model, system, user, temp=0.0, max_tokens=300) -> str:
        self._call_log.append({"method": "chat_json", "model": model, "user": user})
        return self._pick(user)

    async def chat_text(self, model, system, user, temp=0.0, max_tokens=300) -> str:
        self._call_log.append({"method": "chat_text", "model": model, "user": user})
        return self._pick(user)

    async def stream(self, model, messages, cfg) -> AsyncIterator[str]:
        last_user = ""
        for m in reversed(messages):
            if m.get("role") == "user":
                last_user = m.get("content", "")
                break
        self._call_log.append({"method": "stream", "model": model, "user": last_user})
        text = self._pick(last_user)
        for ch in text:
            yield ch


@pytest.fixture
def stub_client():
    return StubClient()


@pytest.fixture
def config():
    cfg = SymbionConfig()
    cfg.llm_provider = "ollama"
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False
    cfg.eval_awareness_enabled = False
    cfg.sandbagging_check_enabled = False
    cfg.reward_hack_check_enabled = False
    cfg.sycophancy_check_enabled = False
    cfg.deception_check_enabled = False
    cfg.sit_awareness_enabled = False
    cfg.frame_acceptance_enabled = False
    cfg.scheming_check_enabled = False
    cfg.swarm_enabled = False
    cfg.test_mode = True
    return cfg
