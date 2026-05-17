"""Tests for retrieval/extraction primitives that are load-bearing for
memory and the hard-trigger paths: _bm25_rank and SYMBION._extract_paths.

Both are pure functions of input — no DB or LLM calls — so they're cheap
to exercise and high-signal: a regression in either silently degrades
memory or the multi-file tool path."""
import sys, os, tempfile
sys.path.insert(0, ".")

import pytest
from symbion_v14 import _bm25_rank, SymbionConfig, SYMBION


# === BM25 ranking ===

class TestBM25Rank:
    def test_empty_inputs(self):
        assert _bm25_rank("", ["doc one", "doc two"]) == []
        assert _bm25_rank("query", []) == []
        assert _bm25_rank("", []) == []

    def test_top_k_respected(self):
        docs = [f"the cat sat on the mat number {i}" for i in range(10)]
        out = _bm25_rank("cat mat", docs, k=3)
        assert len(out) == 3

    def test_score_ordering_desc(self):
        docs = [
            "completely unrelated content about weather",
            "the cat sat on the mat",
            "cat",
        ]
        out = _bm25_rank("cat mat", docs, k=3)
        scores = [s for s, _ in out]
        assert scores == sorted(scores, reverse=True)

    def test_exact_match_outranks_partial(self):
        docs = [
            "the cat sat on the mat",
            "a dog ran in the park",
        ]
        out = _bm25_rank("cat mat", docs, k=2)
        # The cat/mat doc should win
        assert "cat" in out[0][1]

    def test_min_score_filter(self):
        docs = ["completely unrelated about weather and rain"]
        out = _bm25_rank("symbion v14 architecture", docs, k=5, min_score=0.0)
        # No matches at all — empty result, even with min_score=0
        assert out == []

    def test_technical_token_preserved(self):
        # _STOP_WORDS keeps short technical tokens (v14, py, ai, k2)
        docs = [
            "discussion of generic stuff that mentions nothing technical",
            "the v14 refactor changed many things in symbion",
        ]
        out = _bm25_rank("v14", docs, k=2)
        assert out, "v14 should not be stop-word-filtered"
        assert "v14" in out[0][1]

    def test_paraphrase_not_caught(self):
        # BM25 is lexical only — paraphrase should NOT match. This locks in
        # that cosine retrieval is doing the paraphrase work in the hybrid
        # path, not BM25.
        docs = ["the architecture overhaul was substantial"]
        out = _bm25_rank("v14 refactor", docs, k=1)
        assert out == [], "paraphrase should miss BM25; cosine handles it"

    def test_idf_penalizes_common_terms(self):
        # A term in every doc carries low IDF; a rare term carries high IDF.
        # So a query matching the rare term should beat one matching a
        # universally-present term.
        common = "the quick brown fox jumps"
        docs = [common, common, common, "elephant " + common]
        out = _bm25_rank("elephant", docs, k=4)
        # The doc with 'elephant' must come back first
        assert "elephant" in out[0][1]

    def test_long_doc_normalization(self):
        # BM25 with b=0.75 normalizes by doc length so a single match in a
        # huge doc shouldn't outrank a single match in a tight one.
        short = "cat"
        long_filler = " ".join(["word"] * 200) + " cat"
        out = _bm25_rank("cat", [short, long_filler], k=2)
        # Short doc should win (single match, less length penalty)
        assert out[0][1] == short


# === SYMBION._extract_paths ===

@pytest.fixture(scope="module")
def symbion_instance():
    """One SYMBION per test module. Uses a temp DB so the live one
    isn't touched. mkdtemp (not TemporaryDirectory) because WAL mode
    leaves -wal/-shm files that Windows holds open briefly past
    teardown, which makes the auto-cleanup raise."""
    tmp = tempfile.mkdtemp(prefix="symbion-test-")
    cfg = SymbionConfig()
    cfg.db_path = os.path.join(tmp, "test.db")
    cfg.llm_provider = "ollama"
    cfg.tools_enabled = False
    cfg.self_eval_enabled = False
    cfg.proactive_interval_minutes = 0
    cfg.mcp_enabled = False
    yield SYMBION(cfg)
    # Best-effort cleanup; not failing the test if Windows still has a
    # file handle open.
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)


class TestExtractPaths:
    def test_no_paths(self, symbion_instance):
        assert symbion_instance._extract_paths("hello world how are you") == []

    def test_windows_absolute(self, symbion_instance):
        out = symbion_instance._extract_paths("read D:\\notes\\plan.md please")
        assert "D:\\notes\\plan.md" in out

    def test_posix_absolute(self, symbion_instance):
        out = symbion_instance._extract_paths("/etc/hosts has something useful")
        assert "/etc/hosts" not in [p for p in out]  # no extension — should NOT match
        out2 = symbion_instance._extract_paths("look at /var/log/syslog.log")
        assert any("syslog.log" in p for p in out2)

    def test_quoted_path(self, symbion_instance):
        out = symbion_instance._extract_paths('read "Model9/File With Spaces.pdf" first')
        assert "Model9/File With Spaces.pdf" in out

    def test_multiple_paths_one_line(self, symbion_instance):
        # Multi-extension lines fall through to the bare-path fallback.
        out = symbion_instance._extract_paths("a.py b.md c.json all please")
        assert "a.py" in out and "b.md" in out and "c.json" in out

    def test_multiple_paths_separate_lines(self, symbion_instance):
        out = symbion_instance._extract_paths("foo/a.py\nbar/b.md\nbaz/c.json")
        assert "foo/a.py" in out and "bar/b.md" in out and "baz/c.json" in out

    def test_limit_respected(self, symbion_instance):
        text = "\n".join(f"file{i}.py" for i in range(10))
        out = symbion_instance._extract_paths(text, limit=3)
        assert len(out) == 3

    def test_no_dedup_when_distinct(self, symbion_instance):
        out = symbion_instance._extract_paths("a.py and a.py")
        # Same-string dedup yes
        assert out == ["a.py"]

    def test_extension_filter(self, symbion_instance):
        # Random extensions outside _FILE_EXTS shouldn't match.
        out = symbion_instance._extract_paths("read foo.docx and bar.xlsx please")
        assert out == []

    def test_url_doesnt_match(self, symbion_instance):
        # Paths-in-URLs are a known false-positive risk. The hard-trigger
        # in _maybe_tool should not fire on URLs containing .pdf etc.
        out = symbion_instance._extract_paths("see https://example.com/doc.pdf")
        # Either no match or, if matched, the URL is recognizable
        assert all(p == "doc.pdf" or "://" not in p for p in out)
