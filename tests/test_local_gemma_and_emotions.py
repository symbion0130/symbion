"""Regression coverage for the local Gemma provider and emotional check-ins."""
import asyncio
import sys
sys.path.insert(0, ".")

import sqlite3
import zipfile

from fastapi.testclient import TestClient

import symbion_v14 as s
from symbion_v14 import (
    LocalGemmaClient,
    SYMBION,
    SymbionConfig,
    SymbionMemory,
    TurnContext,
    TurnPipeline,
    _load_local_gemma_runtime_config,
    _explicit_work_task,
    _extract_emotion_intensity,
    _intensity_followup_skipped,
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


class _FakeHTTPResponse:
    def __init__(self, payload):
        self._payload = payload

    def raise_for_status(self):
        return None

    def json(self):
        return self._payload


class _FakeStreamResponse:
    def __init__(self):
        self.lines = [
            'data: {"choices":[{"delta":{"content":"hel"}}]}',
            'data: {"choices":[{"delta":{"content":"lo"}}]}',
            "data: [DONE]",
        ]

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        return False

    def raise_for_status(self):
        return None

    async def aiter_lines(self):
        for line in self.lines:
            yield line


class _FakeAsyncClient:
    posts = []
    streams = []

    def __init__(self, *args, **kwargs):
        self.timeout = kwargs.get("timeout")

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        return False

    async def post(self, url, headers=None, json=None):
        self.posts.append({"url": url, "headers": headers, "json": json})
        return _FakeHTTPResponse({
            "choices": [{"message": {"content": "local json ok"}}],
        })

    def stream(self, method, url, headers=None, json=None):
        self.streams.append({
            "method": method, "url": url, "headers": headers, "json": json,
        })
        return _FakeStreamResponse()


def test_local_gemma_chat_json_payload_is_openai_compatible(monkeypatch):
    _FakeAsyncClient.posts = []
    monkeypatch.setattr(s.httpx, "AsyncClient", _FakeAsyncClient)
    cfg = SymbionConfig()
    cfg.local_gemma_json_max_tokens = 123
    client = LocalGemmaClient("local-gemma", cfg, "http://127.0.0.1:8088/v1")

    out = asyncio.run(client.chat_json(
        "override-model", "system text", "user text", temp=0.2, max_tokens=9999))

    assert out == "local json ok"
    req = _FakeAsyncClient.posts[-1]
    assert req["url"] == "http://127.0.0.1:8088/v1/chat/completions"
    assert req["headers"] == {"Content-Type": "application/json"}
    assert req["json"] == {
        "model": "override-model",
        "max_tokens": 123,
        "temperature": 0.2,
        "messages": [
            {"role": "system", "content": "system text"},
            {"role": "user", "content": "user text"},
        ],
    }
    assert "response_format" not in req["json"]


def test_local_gemma_stream_payload_caps_tokens_and_sets_stream(monkeypatch):
    _FakeAsyncClient.streams = []
    monkeypatch.setattr(s.httpx, "AsyncClient", _FakeAsyncClient)
    cfg = SymbionConfig()
    cfg.local_gemma_max_tokens = 77
    cfg.max_tokens = 9999
    client = LocalGemmaClient("local-gemma", cfg, "http://127.0.0.1:8088/v1")

    async def _collect():
        out = []
        async for tok in client.stream("", [{"role": "user", "content": "hi"}], cfg):
            out.append(tok)
        return out

    assert asyncio.run(_collect()) == ["hel", "lo"]
    req = _FakeAsyncClient.streams[-1]
    assert req["method"] == "POST"
    assert req["url"] == "http://127.0.0.1:8088/v1/chat/completions"
    assert req["json"]["model"] == "local-gemma"
    assert req["json"]["max_tokens"] == 77
    assert req["json"]["stream"] is True
    assert req["json"]["messages"] == [{"role": "user", "content": "hi"}]


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


def test_emotional_checkins_are_clamped_scoped_and_skip_neutral(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)

    assert memory.save_emotional_checkin(
        session="s0", user="aaron", emotion="neutral") == 0
    memory.save_emotional_checkin(
        session="s1", user="aaron", emotion="panic", intensity=999,
        valence=-3, note="too high", captured_by="test")
    memory.save_emotional_checkin(
        session="s2", user="lala", emotion="panic", intensity=-20,
        valence=3, note="other user", captured_by="test")

    aaron_rows = memory.get_recent_emotional_checkins("aaron", emotion="panic")
    lala_rows = memory.get_recent_emotional_checkins("lala", emotion="panic")

    assert len(aaron_rows) == 1
    assert aaron_rows[0]["intensity"] == 100
    assert aaron_rows[0]["valence"] == -1.0
    assert aaron_rows[0]["note"] == "too high"
    assert len(lala_rows) == 1
    assert lala_rows[0]["intensity"] == 0
    assert lala_rows[0]["valence"] == 1.0


def test_emotional_history_api_roundtrip(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    cfg.db_path = str(db)
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.api_key = ""
    cfg.shared_learnings_auto_import = False
    app = s.build_web_app(SYMBION(cfg))

    client = TestClient(app)
    r = client.post("/api/emotions", json={
        "session": "web1",
        "user": "Aaron!",
        "emotion": "anxious",
        "intensity": 73,
        "note": "before the demo",
    })
    assert r.status_code == 200
    assert r.json()["id"] > 0

    r = client.get("/api/emotions", params={"user": "aaron", "limit": 5})
    assert r.status_code == 200
    data = r.json()
    assert data["user"] == "aaron"
    assert data["summary"]["count"] == 1
    assert data["summary"]["avg_intensity"] == 73
    assert data["checkins"][0]["emotion"] == "anxious"
    assert data["checkins"][0]["note"] == "before the demo"


def test_emotional_analytics_tracks_graph_ready_signals(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)

    cid = memory.save_emotional_checkin(
        session="s1", user="aaron", emotion="anxious", intensity=82,
        trigger="demo", note="tight chest", stress=91,
        practices_helped=["breathing", "walk"],
        positive_marker="asked for help")
    memory.save_emotional_analytics(
        session="s1", user="aaron", emotion="hopeful", hope=64,
        peace=55, trigger_event="after prayer",
        negative_marker="slept poorly")

    rows = memory.get_emotional_analytics("aaron", limit=5, days=365)
    assert len(rows) == 2
    assert rows[0]["hope"] == 64
    assert rows[0]["negative_marker"] == "slept poorly"
    assert rows[1]["source_checkin_id"] == cid
    assert rows[1]["stress"] == 91
    assert rows[1]["practices_helped"] == ["breathing", "walk"]


def test_emotional_analytics_api_roundtrip(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    cfg.db_path = str(db)
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.api_key = ""
    cfg.shared_learnings_auto_import = False
    app = s.build_web_app(SYMBION(cfg))
    client = TestClient(app)

    r = client.post("/api/emotions", json={
        "session": "web1",
        "user": "aaron",
        "emotion": "calm",
        "intensity": 61,
        "peace": 72,
        "hope": 50,
        "trigger_event": "after a walk",
        "practices_helped": ["walk"],
        "positive_marker": "settled faster",
    })
    assert r.status_code == 200

    r = client.get("/api/emotional-analytics", params={"user": "aaron"})
    assert r.status_code == 200
    data = r.json()
    assert data["signals"][0]["peace"] == 72
    assert data["signals"][0]["practices_helped"] == ["walk"]


def test_emotional_checkin_api_edit_and_delete(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    cfg.db_path = str(db)
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.api_key = ""
    cfg.shared_learnings_auto_import = False
    app = s.build_web_app(SYMBION(cfg))
    client = TestClient(app)

    created = client.post("/api/emotions", json={
        "session": "web1",
        "user": "aaron",
        "emotion": "anxious",
        "intensity": 80,
        "note": "before call",
    }).json()
    cid = created["id"]

    r = client.patch(f"/api/emotions/{cid}", json={
        "user": "aaron",
        "emotion": "calm",
        "intensity": 42,
        "peace": 70,
        "note": "after walk",
    })
    assert r.status_code == 200
    r = client.get("/api/emotions", params={"user": "aaron"})
    rows = r.json()["checkins"]
    assert rows[0]["emotion"] == "calm"
    assert rows[0]["intensity"] == 42
    assert rows[0]["peace"] == 70

    r = client.delete(f"/api/emotions/{cid}", params={"user": "aaron"})
    assert r.status_code == 200
    r = client.get("/api/emotions", params={"user": "aaron"})
    assert r.json()["checkins"] == []
    r = client.get("/api/emotional-analytics", params={"user": "aaron"})
    assert r.json()["signals"] == []


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


def test_emotional_analytics_tool_dispatch_uses_memory(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)
    tools = SymbionTools(memory=memory)

    out = asyncio.run(tools.dispatch(
        "record_emotional_analytics",
        {
            "emotion": "calm",
            "peace": 82,
            "hope": 76,
            "trigger_event": "after prayer",
            "practices_helped": "prayer, stretching",
            "positive_marker": "settled faster",
        },
        cfg,
        active_user="aaron",
        session="s-analytics",
    ))

    rows = memory.get_emotional_analytics("aaron", limit=5)
    assert "recorded" in out
    assert rows[0]["peace"] == 82
    assert rows[0]["hope"] == 76
    assert rows[0]["practices_helped"] == ["prayer", "stretching"]
    assert rows[0]["positive_marker"] == "settled faster"


def test_active_user_memory_search_and_item_reads_are_scoped(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)

    aaron_summary_id = memory.save_summary(
        "s-aaron", "Aaron explored vector database recall for old project notes.", 2,
        user="aaron")
    lala_summary_id = memory.save_summary(
        "s-lala", "Lala explored vector database recall for a private plan.", 2,
        user="lala")
    memory.add("user", "Can you remember the vector database plan?", "s-aaron", user="aaron")
    memory.add("assistant", "We planned a BM25 plus exact item read path.", "s-aaron", user="aaron")
    memory.add("user", "Lala same-session text should not leak.", "s-aaron", user="lala")
    memory.save_technique(
        query="memory recall",
        move="Use source-labeled search results before exact reads.",
        session="s-aaron", user="aaron", source="local")

    rows = memory.search_memory("vector database recall", scope="all", k=10, user="aaron")

    assert any(r["source"] == "summary" and r["id"] == aaron_summary_id for r in rows)
    assert all(not (r["source"] == "summary" and r["id"] == lala_summary_id) for r in rows)
    assert all(r.get("session") != "s-lala" for r in rows)

    owned = memory.get_memory_item("summary", aaron_summary_id, user="aaron")
    blocked = memory.get_memory_item("summary", lala_summary_id, user="aaron")
    assert owned and "vector database" in owned["content"]
    assert blocked is None

    session = memory.read_session("s-aaron", user="aaron", limit=20)
    text = "\n".join(m["content"] for m in session["messages"])
    assert "vector database plan" in text
    assert "should not leak" not in text


def test_related_sessions_profile_fact_and_corrections_are_scoped(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)
    memory.add("user", "We talked about the memory correction workflow.", "s1", user="aaron")
    memory.add("assistant", "We should preserve source sessions.", "s1", user="aaron")
    memory.add("user", "Private unrelated thing.", "s2", user="lala")
    memory.update_profile({"current_projects": ["Symbion native shell"]}, user="aaron")

    sessions = memory.list_related_sessions("memory correction source sessions", user="aaron")
    fact = memory.get_profile_fact("current_projects", user="aaron")
    correction_id = memory.record_memory_correction(
        user="aaron", session="s1", target_source="summary", target_id=123,
        correction_type="sensitive", note="Do not bring this up unless I ask.")

    assert sessions[0]["session"] == "s1"
    assert "Private unrelated" not in sessions[0]["preview"]
    assert fact and fact["value"] == ["Symbion native shell"]
    assert correction_id > 0

    tools = SymbionTools(memory=memory)
    related = asyncio.run(tools.dispatch(
        "list_related_sessions", {"query": "memory correction"}, cfg,
        active_user="aaron", session="s1"))
    profile = asyncio.run(tools.dispatch(
        "get_profile_fact", {"key": "current_projects"}, cfg,
        active_user="aaron", session="s1"))
    correction = asyncio.run(tools.dispatch(
        "record_memory_correction", {"note": "That memory is wrong.", "correction_type": "wrong"}, cfg,
        active_user="aaron", session="s1"))

    assert "memory:session#s1" in related
    assert "memory:profile:current_projects" in profile
    assert "recorded" in correction


def test_correct_memory_suppresses_and_non_destructively_corrects(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)
    tools = SymbionTools(memory=memory)

    corrected_id = memory.save_summary(
        "s-correct", "Aaron said the project codename was Sparrow.", 1,
        user="aaron")
    res = memory.correct_memory(
        "summary", corrected_id, "Correction: the codename is Kestrel.",
        user="aaron")
    assert res["ok"] is True
    item = memory.get_memory_item("summary", corrected_id, user="aaron")
    assert item["correction"] == "Correction: the codename is Kestrel."
    assert any(r["id"] == corrected_id for r in memory.search_memory(
        "Kestrel", scope="summaries", user="aaron"))

    suppressed_id = memory.save_summary(
        "s-private", "This obsolete private memory should be hidden.", 1,
        user="aaron")
    suppress_tool = asyncio.run(tools.dispatch(
        "correct_memory",
        {"source": "summary", "id": suppressed_id,
         "correction": "Do not bring this obsolete private memory up again.",
         "delete": True},
        cfg, active_user="aaron", session="s-private"))

    assert "Memory suppressed" in suppress_tool
    assert memory.get_memory_item("summary", suppressed_id, user="aaron") is None
    assert all(r["id"] != suppressed_id for r in memory.search_memory(
        "obsolete private", scope="summaries", user="aaron"))


class _FakeConsolidationJudge:
    is_degraded = False

    def __init__(self):
        self.system = ""
        self.prompt = ""

    async def chat_text(self, model, system, prompt, temp, max_tokens):
        self.system = system
        self.prompt = prompt
        return (
            "People: Aaron\n"
            "Projects: Orion vector recall\n"
            "Decisions: source sessions stay attached\n"
            "Emotional context: frustrated at first, relieved after the decision\n"
            "Open loops: none\n"
            "Freshness: durable\n"
            "Confidence: high; all source summaries agree\n"
            "Sensitive: no\n"
            "Summary: Aaron kept the Orion recall decisions and emotional arc."
        )


def test_summary_prompts_require_episode_and_consolidation_metadata():
    assert "People:" in s.SUMMARISE_SYSTEM
    assert "Open loops:" in s.SUMMARISE_SYSTEM
    assert "Freshness:" in s.SUMMARISE_SYSTEM
    assert "Sensitive:" in s.SUMMARISE_SYSTEM
    assert "source sessions" in s.CONSOLIDATE_SYSTEM.lower()
    assert "emotional context" in s.CONSOLIDATE_SYSTEM.lower()


def test_consolidation_preserves_source_sessions_user_and_emotional_detail(tmp_path):
    cfg = _isolated_cfg(tmp_path)
    sym = SYMBION(cfg)
    fake = _FakeConsolidationJudge()
    sym._judge_active = lambda: fake

    for session in ("s-one", "s-two", "s-three"):
        sym.memory.save_summary(
            session,
            "Projects: Orion vector recall. Emotional context: frustrated but hopeful.",
            2,
            embedding=[1.0, 0.0, 0.0],
            user="aaron")

    stats = asyncio.run(sym.consolidate_memory(
        similarity_threshold=0.99, min_cluster_size=3, min_age_days=0.0))

    assert stats["clusters_merged"] == 1
    assert "session=s-one" in fake.prompt
    assert "Emotional context" in fake.system
    with sqlite3.connect(cfg.db_path) as c:
        row = c.execute(
            "SELECT content,user FROM summaries WHERE session='consolidated'"
        ).fetchone()
    assert row is not None
    content, user = row
    assert user == "aaron"
    assert "Source sessions:" in content
    for session in ("s-one", "s-two", "s-three"):
        assert session in content
    assert "frustrated at first, relieved" in content


def test_active_user_memory_tools_fail_closed_without_active_user(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)
    memory.save_summary("s1", "The recall tool should require session user context.", 1,
                        user="aaron")
    tools = SymbionTools(memory=memory)

    missing = asyncio.run(tools.dispatch(
        "search_memory", {"query": "recall tool"}, cfg))
    found = asyncio.run(tools.dispatch(
        "search_memory", {"query": "recall tool", "scope": "summaries"}, cfg,
        active_user="aaron", session="s1"))
    unknown_scope = asyncio.run(tools.dispatch(
        "search_memory", {"query": "recall tool", "scope": "unknown"}, cfg,
        active_user="aaron", session="s1"))

    assert "needs session context" in missing
    assert "memory:summary#1" in found
    assert "scope must be" in unknown_scope


def _write_docx(path, paragraphs):
    document_xml = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
        '<w:body>'
        + "".join(
            f"<w:p><w:r><w:t>{p}</w:t></w:r></w:p>"
            for p in paragraphs
        )
        + "</w:body></w:document>"
    )
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("[Content_Types].xml", "")
        z.writestr("word/document.xml", document_xml)


def test_counseling_source_import_tags_and_retrieves_gently(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)
    docx = tmp_path / "MasterDocument.docx"
    _write_docx(docx, [
        "Safe listener practice: mirror gently and ask one question.",
        "Journaling can help grief by naming what still hurts.",
        "Spiritual warfare and demon language is high intensity material.",
    ])

    stats = memory.import_counseling_source_docx(str(docx))
    gentle = memory.search_counseling_sources("safe listener grief journaling", k=5)
    high_default = memory.search_counseling_sources("demon spiritual warfare", k=5)
    high_explicit = memory.search_counseling_sources(
        "demon spiritual warfare", k=5, include_high_intensity=True)

    assert stats["ok"] is True
    assert stats["chunks"] >= 1
    assert any("safe_listener" in r["tags"] or "journaling" in r["tags"] for r in gentle)
    assert all(r["intensity"] != "high" for r in high_default)
    assert any(r["intensity"] == "high" for r in high_explicit)

    tools = SymbionTools(memory=memory)
    out = asyncio.run(tools.dispatch(
        "search_counseling_sources", {"query": "safe listener", "k": 2}, cfg,
        active_user="aaron", session="s1"))
    assert "Counseling source chunks" in out
    assert "guidance only" in out


def test_extract_emotion_intensity_normalizes_ten_point_scale():
    assert _extract_emotion_intensity("anger is at 7/10") == 70
    assert _extract_emotion_intensity("intensity 93") == 93
    assert _extract_emotion_intensity("no number here") is None


class _ResponderStub:
    supports_tools = True
    cb = None

    async def stream_with_tools(self, model, messages, tools, cfg, tool_executor,
                                 max_iterations=8, max_tool_chars=80_000,
                                 show_reasoning=None):
        yield {"type": "text", "text": "steady response"}
        yield {"type": "done", "iterations": 1, "stop_reason": "end_turn",
               "tool_calls": []}

    async def stream(self, model, messages, cfg):
        for ch in "steady response":
            yield ch


def _isolated_cfg(tmp_path, provider="anthropic"):
    cfg = SymbionConfig()
    cfg.llm_provider = provider
    cfg.anthropic_api_key = "test-key"
    cfg.db_path = str(tmp_path / "symbion.db")
    cfg.log_path = str(tmp_path / "symbion.log")
    cfg.self_eval_enabled = False
    cfg.embedding_enabled = False
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    cfg.shared_learnings_auto_import = False
    cfg.fallback_chain = []
    return cfg


def _assembled_prompt_for(tmp_path, text, emotional_state):
    sym = SYMBION(_isolated_cfg(tmp_path))
    ctx = TurnContext(text=text, session="prompt-test")
    ctx.active_user = "aaron"
    ctx.agent_loop_active = True
    ctx.evaluation = {"should_assist": True, "over_cautious": False}
    ctx.emotional_state = emotional_state
    TurnPipeline(sym, ctx).assemble_system_prompt()
    return ctx.system


def test_emotional_mode_prompt_pins_no_list_one_question_constraints(tmp_path):
    prompt = _assembled_prompt_for(
        tmp_path,
        "I feel ashamed and stuck and I need to talk this through.",
        {"state": "sad", "suggested_response_mode": "gentle_slow"},
    )

    assert "Emotional processing mode" in prompt
    assert "one brief mirror or tentative label" in prompt
    assert "exactly one simple follow-up question" in prompt
    assert "No bullet lists" in prompt
    assert "no multi-step plans" in prompt
    assert "No bullet points unless asked" in prompt


def test_emotional_mode_adds_gentle_optional_intensity_followup(tmp_path):
    prompt = _assembled_prompt_for(
        tmp_path,
        "I feel anxious and ashamed and I need to talk this through.",
        {"state": "anxious", "suggested_response_mode": "gentle_slow"},
    )

    assert "Emotional processing mode" in prompt
    assert "exactly one simple follow-up question" in prompt
    assert "gentle optional intensity check" in prompt
    assert "make clear they can skip it" in prompt


def test_emotional_mode_respects_intensity_skip_path(tmp_path):
    prompt = _assembled_prompt_for(
        tmp_path,
        "I feel anxious, but don't ask me to rate the intensity.",
        {"state": "anxious", "suggested_response_mode": "gentle_slow"},
    )

    assert "Emotional processing mode" in prompt
    assert "gentle optional intensity check" not in prompt
    assert "already skipped numeric intensity tracking" in prompt
    assert "do not ask them to rate or number it" in prompt


def test_emotional_mode_does_not_block_explicit_code_task(tmp_path):
    prompt = _assembled_prompt_for(
        tmp_path,
        "I feel overwhelmed. Please write a Python script that cleans this CSV.",
        {"state": "anxious", "suggested_response_mode": "gentle_slow"},
    )

    assert "Response mode: The person is carrying something heavy" in prompt
    assert "Explicit work mode" in prompt
    assert "Complete the requested work directly" in prompt
    assert "Be as long as the deliverable requires" in prompt
    assert "Emotional processing mode" not in prompt
    assert "exactly one simple follow-up question" not in prompt


def test_explicit_work_detector_covers_writing_without_overmatching_feelings():
    assert _explicit_work_task("Please draft a short release note.")
    assert _explicit_work_task("Can you fix this test failure?")
    assert not _explicit_work_task("You make me feel sad.")


def test_intensity_skip_detector_handles_number_opt_outs():
    assert _intensity_followup_skipped("I feel anxious, but don't ask me to rate it.")
    assert _intensity_followup_skipped("Skip the 0-100 number; I just want to talk.")
    assert not _intensity_followup_skipped("anger is at 7/10")


def test_emotional_mode_constraints_do_not_override_structural_tasks(tmp_path):
    prompt = _assembled_prompt_for(
        tmp_path,
        "I feel lost. Please fix this traceback:\n```py\nraise RuntimeError()\n```",
        {"state": "sad", "suggested_response_mode": "gentle_slow"},
    )

    assert "Response mode: The person is carrying something heavy" in prompt
    assert "Explicit work mode" in prompt
    assert "Emotional processing mode" not in prompt
    assert "exactly one simple follow-up question" not in prompt


def test_respond_persists_detector_emotional_checkin_without_live_llm(tmp_path):
    cfg = _isolated_cfg(tmp_path)
    sym = SYMBION(cfg)
    sym._responder_client = lambda: _ResponderStub()
    sym._should_skip_pregen = lambda text: False
    sym._maybe_tool = lambda text, active_user="", session="": _instant("")

    async def _pregen(text):
        return (
            {"should_assist": True, "human_benefit_score": 0.8,
             "confidence": 0.9, "flags": [], "reasoning": "",
             "over_cautious": False, "escalate": False,
             "escalate_reason": "", "evaluator_degraded": False},
            {"state": "anxious", "confidence": 0.91,
             "suggested_response_mode": "gentle_slow"},
        )
    sym._pre_gen_analysis = _pregen

    asyncio.run(sym.respond("I feel anxious at 8/10 before the call.", "s-detector"))

    rows = sym.memory.get_recent_emotional_checkins("aaron", emotion="anxious")
    assert len(rows) == 1
    assert rows[0]["intensity"] == 80
    assert rows[0]["confidence"] == 0.91
    assert rows[0]["captured_by"] == "detector"
    assert rows[0]["note"] == "I feel anxious at 8/10 before the call."


async def _instant(value):
    return value


def test_local_gemma_runtime_config_reads_codecat_json(tmp_path):
    runtime = tmp_path / "runtime"
    cfg_dir = runtime / "config"
    model_dir = runtime / "models"
    cfg_dir.mkdir(parents=True)
    model_dir.mkdir()
    model = model_dir / "gemma.gguf"
    model.write_text("stub", encoding="utf-8")
    cfg_file = cfg_dir / "codecat.server.json"
    cfg_file.write_text(
        '{"host":"127.0.0.1","port":8088,"model":"runtime/models/gemma.gguf","contextSize":4096}',
        encoding="utf-8",
    )
    cfg = SymbionConfig()
    cfg.local_gemma_codecat_config = str(cfg_file)

    data = _load_local_gemma_runtime_config(cfg)

    assert data["ok"] is True
    assert data["context_tokens"] == 4096
    assert data["model_exists"] is True


def test_local_gemma_context_budget_trims_preamble(tmp_path):
    db = tmp_path / "symbion.db"
    cfg = SymbionConfig()
    cfg.llm_provider = "local_gemma"
    cfg.local_gemma_context_char_budget = 900
    cfg.local_gemma_recent_turns = 3
    init_db(str(db))
    memory = SymbionMemory(str(db), cfg)

    recent = [{"role": "user", "content": str(i)} for i in range(10)]
    parts = ["Current time: Tuesday", "A" * 2000, "B" * 2000]
    out_recent, out_parts = memory._apply_local_gemma_context_budget(recent, parts)

    assert len(out_recent) == 3
    assert len("\n\n".join(out_parts)) <= 900
    assert out_parts[0].startswith("Current time")
