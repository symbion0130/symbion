"""Tests for SymbionTools: calculator, file sandbox, SSRF protection, _parse_json."""
import sys, os, json, tempfile
sys.path.insert(0, ".")

import pytest
from pathlib import Path
from symbion_v13 import SymbionTools, _safe_calc, _is_safe_url, _parse_json, _resolve_in_workspace


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

    def test_read_escape_blocked(self, tmp_path):
        tools = SymbionTools(str(tmp_path))
        result = tools.read_file("../../etc/passwd")
        assert "Error" in result

    def test_write_escape_blocked(self, tmp_path):
        tools = SymbionTools(str(tmp_path))
        result = tools.write_file("/tmp/pwned", "x")
        assert "Error" in result


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
