"""Regression coverage for the local Gemma provider and emotional check-ins."""
import sys
sys.path.insert(0, ".")

import sqlite3

from symbion_v14 import (
    LocalGemmaClient,
    SymbionConfig,
    SymbionMemory,
    _extract_emotion_intensity,
    init_db,
)
from symbion_tools import SymbionTools


def test_local_gemma_client_uses_openai_compatible_endpoint():
    cfg = SymbionConfig()
    client = LocalGemmaClient("local-gemma", cfg, "http://127.0.0.1:8088/v1")

    assert client.model == "local-gemma"
    assert client._url == "http://127.0.0.1:8088/v1/chat/completions"
    assert client._h() == {"Content-Type": "application/json"}
    assert client._cap(999999) == cfg.local_gemma_max_tokens


def test_emotional_checkins_table_and_roundtrip(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)

    cid = memory.save_emotional_checkin(
        session="s1",
        user="aaron",
        emotion="anxious",
        intensity=87,
        note="tight chest before a hard call",
        confidence=1.0,
        captured_by="test",
    )

    assert cid > 0
    rows = memory.get_recent_emotional_checkins("aaron", emotion="anx", limit=5)
    assert len(rows) == 1
    assert rows[0]["emotion"] == "anxious"
    assert rows[0]["intensity"] == 87

    with sqlite3.connect(db) as c:
        count = c.execute("SELECT COUNT(*) FROM emotional_checkins").fetchone()[0]
    assert count == 1


def test_emotional_tool_dispatch_uses_memory(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    tools = SymbionTools(memory=SymbionMemory(str(db), cfg))

    out = tools.record_emotional_checkin(
        "grieving", intensity=70, note="missing dad", session="s2", user="aaron")
    history = tools.search_emotional_history("grieving", user="aaron")

    assert "recorded" in out
    assert "grieving" in history
    assert "missing dad" in history


def test_extract_emotion_intensity_normalizes_ten_point_scale():
    assert _extract_emotion_intensity("anger is at 7/10") == 70
    assert _extract_emotion_intensity("intensity 93") == 93
    assert _extract_emotion_intensity("no number here") is None
