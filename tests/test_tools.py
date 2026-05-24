"""Tests for SymbionTools: calculator, file sandbox, SSRF protection, _parse_json."""
import sys, os, json, tempfile
sys.path.insert(0, ".")

import pytest
from pathlib import Path
from symbion_v14 import SymbionTools, _safe_calc, _is_safe_url, _parse_json, _resolve_in_workspace, _SELF_SOURCE_RE, _SELF_REVIEW_RE, SymbionMemory


# === 4.1: AST-based calculator ===

class TestCalculator:
    def test_basic_arithmetic(self):
        assert _safe_calc("2+2") == "4"

    def test_multiplication(self):
        assert _safe_calc("17 * 23") == "391"

    def test_division(self):
        assert _safe_calc("10 / 3") == str(10/3)

    def test_power(self):
        assert _safe_calc("2^10") == "1024"
        assert _safe_calc("2**10") == "1024"

    def test_sqrt(self):
        assert _safe_calc("sqrt(16)") == "4.0"

    def test_trig(self):
        assert _safe_calc("sin(0)") == "0.0"

    def test_combined(self):
        assert _safe_calc("sqrt(16) + sin(0)") == "4.0"

    def test_pi(self):
        result = float(_safe_calc("pi"))
        assert abs(result - 3.14159265) < 0.001

    def test_floor_ceil(self):
        assert _safe_calc("floor(3.7)") == "3"
        assert _safe_calc("ceil(3.2)") == "4"

    def test_import_attack(self):
        result = _safe_calc("__import__('os').system('ls')")
        assert "Error" in result

    def test_class_escape(self):
        result = _safe_calc("(1).__class__.__bases__")
        assert "Error" in result

    def test_string_literal(self):
        result = _safe_calc("'hello'")
        assert "Error" in result

    def test_lambda(self):
        result = _safe_calc("(lambda: 1)()")
        assert "Error" in result

    def test_list_comp(self):
        result = _safe_calc("[x for x in range(10)]")
        assert "Error" in result

    # --- DoS guards on the Pow path ---

    def test_nested_pow_rejected(self):
        # The classic 9**9**9 attack: Python evaluates the right side first,
        # so this is a tower, not 729**9. Must be refused outright.
        result = _safe_calc("9**9**9")
        assert "Error" in result
        assert "nested" in result.lower()

    def test_nested_pow_caret_form(self):
        result = _safe_calc("9^9^9")
        assert "Error" in result

    def test_left_nested_pow_rejected(self):
        # Left-associative form (a**b)**c — exponent c is small, inner
        # exponent is small, but the intermediate a**b can already be a
        # DoS. Pre-fix only the right-nested form was caught.
        result = _safe_calc("(999**999)**999")
        assert "Error" in result
        assert "nested" in result.lower()

    def test_intermediate_size_capped(self):
        # No nested **, but the single-Pow result blows the bit_length cap.
        # Exercises the dynamic per-step magnitude check rather than the
        # static nested-** check.
        result = _safe_calc("sqrt(10**1500)")
        assert "Error" in result

    def test_exponent_cap(self):
        result = _safe_calc("2**5000")
        assert "Error" in result
        assert "exponent" in result.lower()

    def test_exponent_within_cap_ok(self):
        # Just below the 1000 cap should still work.
        result = _safe_calc("2**100")
        assert result == str(2**100)

    def test_literal_magnitude_cap(self):
        # A literal larger than 10**50 should be refused before any op runs.
        big = "1" + "0" * 60  # 10**60
        result = _safe_calc(big)
        assert "Error" in result
        assert "literal" in result.lower()

    def test_result_magnitude_cap(self):
        # Within input limits but result bit_length exceeds 4096 → refused.
        # 2**4097 has bit_length 4098, but the exponent cap (1000) prevents it.
        # Use a flat product instead: 10**300 * 10**300 has ~2000 bits — fine.
        # Use 999**999 (under 1000 cap) which has ~9970 bits.
        result = _safe_calc("999**999")
        assert "Error" in result
        assert "result" in result.lower()


# === 4.2: Workspace sandbox ===

class TestWorkspaceSandbox:
    def test_resolve_normal(self, tmp_path):
        (tmp_path / "notes.txt").write_text("hello")
        p = _resolve_in_workspace("notes.txt", tmp_path)
        assert p == tmp_path / "notes.txt"

    def test_reject_parent_escape(self, tmp_path):
        with pytest.raises(ValueError, match="escapes workspace"):
            _resolve_in_workspace("../../etc/passwd", tmp_path)

    def test_reject_absolute(self, tmp_path):
        with pytest.raises(ValueError, match="escapes workspace"):
            _resolve_in_workspace("/tmp/pwned", tmp_path)

    def test_read_write_in_workspace(self, tmp_path):
        tools = SymbionTools(str(tmp_path))
        result = tools.write_file("test.txt", "hello world")
        assert "Written" in result
        content = tools.read_file("test.txt")
        assert "hello world" in content

    def test_read_escape_allowed(self, tmp_path):
        # Reads are intentionally unrestricted — the user opted in to
        # machine-wide read access. The sandbox only applies to writes.
        # We verify the path resolves outward without raising; the file
        # itself most likely doesn't exist on the test box, so a "Not
        # found" response is the success case here.
        tools = SymbionTools(str(tmp_path))
        result = tools.read_file("../../etc/passwd")
        assert "outside" not in result.lower() and "sandbox" not in result.lower()

    def test_write_machine_wide_allowed(self, tmp_path):
        # As of 2026-05-22 writes are machine-wide, mirroring reads. An
        # absolute path under tmp_path is allowed and the file appears
        # at exactly that location.
        tools = SymbionTools(str(tmp_path))
        target = tmp_path / "outside_workspace" / "file.txt"
        result = tools.write_file(str(target), "machine-wide ok")
        assert "Written" in result
        assert target.exists()
        assert target.read_text() == "machine-wide ok"

    def test_read_file_anchors_line_and_char_counts(self, tmp_path):
        # 2026-05-24 anti-confab: every read_file response must lead with a
        # `[file: NAME | N lines | M chars | ...]` header so the model has
        # a ground-truth anchor and can't eyeball-and-guess. Locked in
        # after a self-review where Symbion read symbion_v14.py (9,303
        # lines) via the agent loop and asserted "~4500 lines" anyway.
        tools = SymbionTools(str(tmp_path))
        tools.write_file("three.txt", "a\nb\nc\n")
        out = tools.read_file("three.txt")
        first = out.splitlines()[0]
        assert first.startswith("[file: three.txt |")
        assert "3 lines" in first
        assert "6 chars" in first
        assert "full read" in first
        # File content itself still arrives intact.
        assert "a\nb\nc" in out

    def test_read_file_chunk_header_reports_chunk_range(self, tmp_path):
        # Partial reads carry the same anchored totals plus the chunk range
        # the caller actually got. Without this, multi-chunk reviews lose
        # track of which slice they're looking at.
        tools = SymbionTools(str(tmp_path))
        body = "\n".join(f"row {i}" for i in range(50))  # 50 lines, no trailing newline
        tools.write_file("rows.txt", body)
        out = tools.read_file_chunk("rows.txt", offset=0, max_chars=30)
        first = out.splitlines()[0]
        assert first.startswith("[file: rows.txt |")
        assert "50 lines" in first
        assert f"{len(body)} chars" in first
        assert "chunk 0-30" in first
        # Partial read also keeps the explicit "more remaining" tail so the
        # model knows to call read_file_chunk again instead of inferring.
        assert "chars remaining" in out
        assert "offset=30" in out


# === 4.3: SSRF protection ===

class TestSSRF:
    def test_https_allowed(self):
        ok, _ = _is_safe_url("https://example.com")
        assert ok

    def test_http_allowed(self):
        ok, _ = _is_safe_url("http://example.com")
        assert ok

    def test_file_scheme_blocked(self):
        ok, reason = _is_safe_url("file:///etc/passwd")
        assert not ok
        assert "scheme" in reason.lower()

    def test_localhost_blocked(self):
        ok, reason = _is_safe_url("http://localhost:8080/admin")
        assert not ok

    def test_metadata_blocked(self):
        ok, _ = _is_safe_url("http://metadata.google.internal/latest/")
        assert not ok

    def test_ftp_blocked(self):
        ok, _ = _is_safe_url("ftp://evil.com/file")
        assert not ok

    # --- regression: IP-literal AWS metadata host ---
    def test_aws_metadata_ip_literal_blocked(self):
        # Pre-fix, a literal IP would only be caught by getaddrinfo's
        # ip.is_link_local check. Now it's caught directly as an IP literal
        # before any DNS lookup.
        ok, reason = _is_safe_url("http://169.254.169.254/latest/meta-data/")
        assert not ok
        assert "169.254" in reason

    def test_loopback_ip_literal_blocked(self):
        ok, _ = _is_safe_url("http://127.0.0.1:8080/admin")
        assert not ok

    def test_private_ip_literal_blocked(self):
        ok, _ = _is_safe_url("http://10.0.0.1/")
        assert not ok

    # --- regression: DNS-fail now rejects rather than allows ---
    def test_dns_failure_rejected(self):
        # .invalid is RFC 6761 reserved and guaranteed never to resolve.
        # Pre-fix this returned (True, "ok") because gaierror was swallowed;
        # post-fix it must fail closed.
        ok, reason = _is_safe_url("http://nonexistent-host.invalid/")
        assert not ok
        assert "dns" in reason.lower() or "resolution" in reason.lower()


# === 4.4: _parse_json ===

class TestParseJson:
    def test_clean_json(self):
        r = _parse_json('{"a": 1}', {"a": 0})
        assert r == {"a": 1}

    def test_json_with_prose(self):
        r = _parse_json('Here is the result: {"a": 1} done.', {"a": 0})
        assert r == {"a": 1}

    def test_json_with_string_braces(self):
        r = _parse_json('{"msg": "use {x} here", "ok": true}', {})
        assert r["msg"] == "use {x} here"
        assert r["ok"] is True

    def test_malformed(self):
        r = _parse_json('not json at all', {"default": True})
        assert r == {"default": True}

    def test_empty(self):
        r = _parse_json('', {"empty": True})
        assert r == {"empty": True}

    def test_code_fence(self):
        r = _parse_json('```json\n{"a": 1}\n```', {"a": 0})
        assert r == {"a": 1}


# === 4.5: Self-source and self-review pre-fetch trigger regexes ===
#
# Two regexes, two cost tiers (split 2026-05-24 after the unified version
# caused 429s):
#   _SELF_SOURCE_RE  -- narrow trigger for queries that explicitly want the
#                       source code. Pre-fetch injects manifest + full
#                       symbion_v14.py (~140K tokens). Cost justified by
#                       grounding need.
#   _SELF_REVIEW_RE  -- broader trigger for "self review" / "audit yourself"
#                       style queries. Pre-fetch injects ONLY the manifest
#                       (~1KB); model uses tools to pull source if needed.
#                       Avoids the 450K-input-tokens/min org-limit thrash.

class TestSelfSourceRegex:
    """Source-wanting queries -- always trigger _SELF_SOURCE_RE. The
    pre-fetch will inject manifest + full source on these."""

    @pytest.mark.parametrize("query", [
        "explain symbion_v14.py to me",
        "walk me through your code",
        "what's in your codebase?",
        "show me your architecture",
        "your pipeline has a bug",
        "walk me through respond()",
        "review the symbion pipeline",
    ])
    def test_source_wanting_queries_match(self, query):
        assert _SELF_SOURCE_RE.search(query) is not None, f"should match: {query!r}"

    @pytest.mark.parametrize("query", [
        # Clearly user-directed -- not about Symbion's own code.
        "review my code",
        "audit this PR I'm sending",
        "what's in this file I attached",
        "explain how python works",
        "review the changes I just pushed",
        "audit my database schema",
        # Self-review patterns -- these belong to _SELF_REVIEW_RE,
        # NOT _SELF_SOURCE_RE. Verify they DON'T trigger source injection.
        "self review and tell me what you'd change",
        "review yourself",
        "audit yourself end to end",
    ])
    def test_source_regex_does_not_match(self, query):
        assert _SELF_SOURCE_RE.search(query) is None, f"should NOT match: {query!r}"


class TestSelfReviewRegex:
    """Self-evaluation queries -- trigger _SELF_REVIEW_RE only. Pre-fetch
    will inject manifest-only (cheap, ~1KB) and let the agent loop pull
    source on demand if needed."""

    @pytest.mark.parametrize("query", [
        "self review and tell me what you'd change",
        "self-review pls",
        "self audit your decisions",
        "self critique your last answer",
        "self reflect on the last hour",
        "self assessment please",
        "review yourself",
        "audit yourself end to end",
        "critique yourself",
        "assess yourself honestly",
    ])
    def test_review_queries_match(self, query):
        assert _SELF_REVIEW_RE.search(query) is not None, f"should match: {query!r}"

    @pytest.mark.parametrize("query", [
        # User-directed -- review of user's own work, not Symbion's.
        "review my code",
        "audit this PR I'm sending",
        "review the changes I just pushed",
        # Source-wanting -- these belong to _SELF_SOURCE_RE, not review.
        "walk me through respond()",
        "what's in your codebase?",
        "explain symbion_v14.py",
    ])
    def test_review_regex_does_not_match(self, query):
        assert _SELF_REVIEW_RE.search(query) is None, f"should NOT match: {query!r}"


# === 4.6: shared_learnings.md import integrity ===
#
# Defensive checks on _parse_shared_learnings_file (added 2026-05-24
# followup). Imported techniques surface in future system prompts via
# the techniques retrieval block, so anyone who can write the shared
# file controls a prompt-injection vector.

def _write_block(path: Path, header: str, query: str, move: str,
                 evidence: str = "") -> None:
    """Helper: append one ## block to the shared_learnings.md file at path."""
    parts = [f"## {header}", f"**query:** {query}", f"**move:** {move}"]
    if evidence:
        parts.append(f"**evidence:** {evidence}")
    path.write_text((path.read_text() if path.exists() else "") +
                    "\n" + "\n\n".join(parts) + "\n---\n",
                    encoding="utf-8")


class TestSharedLearningsImportIntegrity:
    def test_normal_entry_parses_cleanly(self, tmp_path):
        """A well-formed entry round-trips: hash matches, fields preserved."""
        path = tmp_path / "shared.md"
        user = "aaron"; query = "how to skim a PDF"
        move = "skim TOC then conclusion to map argument shape"
        h = SymbionMemory._technique_hash(user, query, move)
        _write_block(path, f"2026-05-24 · {user} · hash:{h}",
                     query, move, "evidence text")
        entries = SymbionMemory._parse_shared_learnings_file(str(path))
        assert len(entries) == 1
        assert entries[0]["user"] == user
        assert entries[0]["move"] == move
        assert entries[0]["query"] == query
        assert entries[0]["hash"] == h

    def test_file_over_size_cap_refuses(self, tmp_path, monkeypatch):
        """File-size cap kicks in before content parsing -- defends
        against a giant file stuffing the prompt at import time."""
        path = tmp_path / "huge.md"
        # Write a small file but spoof its size via stat override --
        # cheaper than writing 10MB.
        path.write_text("## 2026 · aaron · hash:abc\n**move:** test\n",
                        encoding="utf-8")
        real_stat = Path.stat
        def fake_stat(self):
            r = real_stat(self)
            class _S:
                st_size = SymbionMemory._SHARED_MAX_FILE_BYTES + 1
            return _S()
        monkeypatch.setattr(Path, "stat", fake_stat)
        entries = SymbionMemory._parse_shared_learnings_file(str(path))
        assert entries == [], "oversized file should be refused entirely"

    def test_hash_mismatch_skipped(self, tmp_path):
        """Hash in the header that doesn't match recomputed (user, query,
        move) -- tamper or corruption signature; entry must be dropped."""
        path = tmp_path / "shared.md"
        _write_block(path,
                     "2026-05-24 · aaron · hash:deadbeef0000",  # bogus hash
                     "original query", "the original move sentence")
        entries = SymbionMemory._parse_shared_learnings_file(str(path))
        assert entries == [], "hash-mismatched entry should be skipped"

    def test_missing_hash_recomputed_not_skipped(self, tmp_path):
        """Legacy entries without a hash header still parse -- hash is
        derived from content. Doesn't trigger the mismatch path."""
        path = tmp_path / "shared.md"
        _write_block(path, "2026-05-24 · aaron",  # no hash segment
                     "what is X", "explain X via the simplest case")
        entries = SymbionMemory._parse_shared_learnings_file(str(path))
        assert len(entries) == 1
        assert entries[0]["hash"] == SymbionMemory._technique_hash(
            "aaron", "what is X", "explain X via the simplest case")

    @pytest.mark.parametrize("marker_field,marker", [
        ("move",     "[TOOL_DATA"),
        ("move",     "[/TOOL_DATA]"),
        ("move",     "[SYMBION_REVISE]"),
        ("query",    "[THINKING_START]"),
        ("evidence", "[THINKING_END]"),
    ])
    def test_injection_markers_rejected(self, tmp_path, marker_field, marker):
        """Entries containing Symbion's own system markers get rejected.
        A legitimate move/query/evidence never contains [TOOL_DATA etc
        as content; presence is the obvious prompt-injection signature."""
        path = tmp_path / "shared.md"
        fields = {"query": "normal query",
                  "move":  "normal move sentence describing the technique",
                  "evidence": "normal evidence"}
        fields[marker_field] = fields[marker_field] + " " + marker
        user = "aaron"
        h = SymbionMemory._technique_hash(user, fields["query"], fields["move"])
        _write_block(path, f"2026-05-24 · {user} · hash:{h}",
                     fields["query"], fields["move"], fields["evidence"])
        entries = SymbionMemory._parse_shared_learnings_file(str(path))
        assert entries == [], (
            f"entry with {marker!r} in {marker_field} should be rejected"
        )

    def test_oversized_fields_truncated(self, tmp_path):
        """Fields over the per-field caps get truncated to the cap.
        Verifies the caps hold (not just by failing -- by landing at
        exactly the cap length)."""
        path = tmp_path / "shared.md"
        long_move = "x" * (SymbionMemory._SHARED_MAX_MOVE + 200)
        long_query = "q" * (SymbionMemory._SHARED_MAX_QUERY + 200)
        # Write WITHOUT a hash header so the truncation-vs-hash mismatch
        # doesn't drop the entry (would do the right thing in production
        # but obscures what's being tested here).
        _write_block(path, "2026-05-24 · aaron",
                     long_query, long_move, "x" * 5000)
        entries = SymbionMemory._parse_shared_learnings_file(str(path))
        assert len(entries) == 1
        assert len(entries[0]["move"]) == SymbionMemory._SHARED_MAX_MOVE
        assert len(entries[0]["query"]) == SymbionMemory._SHARED_MAX_QUERY
        assert len(entries[0]["evidence"]) == SymbionMemory._SHARED_MAX_EVIDENCE
