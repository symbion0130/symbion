import os
import sqlite3
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, ".")

from symbion_v14 import SymbionConfig, SymbionMemory, init_db


def _write_docx(path: Path, paragraphs):
    def p(text):
        return (
            '<w:p><w:r><w:t xml:space="preserve">'
            + text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            + "</w:t></w:r></w:p>"
        )

    document = (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
        "<w:body>"
        + "".join(p(x) for x in paragraphs)
        + "</w:body></w:document>"
    )
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("[Content_Types].xml", "<Types/>")
        z.writestr("word/document.xml", document)


def _memory(tmp_path):
    db = tmp_path / "test.db"
    init_db(str(db))
    cfg = SymbionConfig()
    cfg.db_path = str(db)
    cfg.counseling_source_auto_import = False
    return SymbionMemory(str(db), cfg), db


def test_init_db_adds_counseling_source_runtime_columns(tmp_path):
    _, db = _memory(tmp_path)
    with sqlite3.connect(db) as c:
        cols = {r[1] for r in c.execute("PRAGMA table_info(counseling_sources)")}
    assert {"source_hash", "chunk_index", "tags", "intensity", "safety_class", "preference"} <= cols


def test_import_docx_chunks_tags_and_retrieves_gentle_practical(tmp_path):
    memory, _ = _memory(tmp_path)
    docx = tmp_path / "MasterDocument.docx"
    grounding = "Grounding practice: breathe slowly, name the feeling, drink water, and take one next step. " * 12
    high = "Spiritual warfare and demon enemy language belongs in high intensity source review, not default support. " * 12
    _write_docx(docx, [grounding, high])

    stats = memory.import_counseling_source_docx(str(docx))
    assert stats["ok"] is True
    assert stats["inserted"] == 2

    hits = memory.search_counseling_sources("I feel anxious and need grounding", k=3)
    assert hits
    assert all(h["intensity"] != "high" for h in hits)
    assert "grounding" in hits[0]["tags"]
    assert hits[0]["preference"] == "gentle_practical"


def test_high_intensity_chunks_are_not_default_but_can_be_reviewed(tmp_path):
    memory, _ = _memory(tmp_path)
    docx = tmp_path / "MasterDocument.docx"
    grounding = "Grounding practice: breathe slowly and notice the room. " * 20
    high = "Demon spiritual warfare enemy deliverance language should be tagged high intensity. " * 20
    _write_docx(docx, [grounding, high])
    memory.import_counseling_source_docx(str(docx))

    default_hits = memory.search_counseling_sources("spiritual warfare", k=5)
    assert all(h["intensity"] != "high" for h in default_hits)

    review_hits = memory.search_counseling_sources(
        "spiritual warfare", k=5, include_high_intensity=True)
    assert any(h["intensity"] == "high" for h in review_hits)


def test_crisis_queries_do_not_retrieve_source_chunks(tmp_path):
    memory, _ = _memory(tmp_path)
    docx = tmp_path / "MasterDocument.docx"
    crisis = "If suicide or self-harm appears, ask a plain safety question and get urgent local help. " * 20
    _write_docx(docx, [crisis])
    memory.import_counseling_source_docx(str(docx))

    assert memory.search_counseling_sources("I might kill myself tonight", k=5) == []

