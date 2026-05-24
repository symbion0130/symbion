"""SymbionTools — file/data/web/location/cross-user tools for the agent loop.

Extracted from symbion_v14.py on 2026-05-24. Lives as a flat module
alongside symbion_v14.py to avoid the symbion/__init__.py re-export
cycle (the package's __init__ imports from symbion_v14, so anything
symbion_v14 imports from symbion.* would recurse).

What's here:
  - _safe_calc / _CalcError / _eval_calc_node — AST-validated calculator
  - _is_safe_url — SSRF guard
  - _resolve_in_workspace — path resolver (machine_wide=True default)
  - TOOL_SCHEMAS — Anthropic-format schemas for the agent loop
  - SymbionTools — the class itself (13 tools + dispatch + _validate_args)

External seams (already abstract, no leaks back into Symbion internals):
  - cfg: SymbionConfig passed to dispatch (string-quoted to avoid import)
  - memory: SymbionMemory injected via __init__ (string-quoted; only the
    cross-user + technique tools use it)
  - responder + responder_model passed to dispatch for read_image
"""
from __future__ import annotations

import ast as _ast
import logging
import math as _math
import re
import socket as _socket
import urllib.parse
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import httpx
    _HTTPX = True
except ImportError:
    _HTTPX = False

logger = logging.getLogger("symbion")


# ==============================================================================
#  SAFE CALCULATOR (AST-validated, no eval)
# ==============================================================================

_CALC_ALLOWED_NODES = (
    _ast.Expression, _ast.BinOp, _ast.UnaryOp, _ast.Constant,
    _ast.Call, _ast.Name, _ast.Load,
    _ast.Add, _ast.Sub, _ast.Mult, _ast.Div, _ast.Mod, _ast.Pow,
    _ast.FloorDiv, _ast.USub, _ast.UAdd,
)
_CALC_ALLOWED_FUNCS = {
    "sqrt": _math.sqrt, "sin": _math.sin, "cos": _math.cos, "tan": _math.tan,
    "log": _math.log, "abs": abs, "round": round, "floor": _math.floor, "ceil": _math.ceil,
}
_CALC_ALLOWED_NAMES = {"pi": _math.pi, "e": _math.e}

_CALC_MAX_EXPONENT = 1000      # caps |b| in a**b (literal *or* computed)
_CALC_MAX_LITERAL  = 10 ** 50  # caps any single numeric literal magnitude
_CALC_MAX_BITS     = 4096      # caps every intermediate int's bit_length


class _CalcError(Exception):
    """Raised inside _eval_calc_node when a DoS guard trips."""


def _eval_calc_node(node):
    """Recursively evaluate a validated calc AST, capping size at every step.

    Manual evaluation (no eval()/compile()) lets us refuse intermediate
    blow-ups like (999**999)**999 — which sail past static literal-exponent
    checks because the outer exponent is small and the inner is computed.
    """
    if isinstance(node, _ast.Constant):
        return node.value
    if isinstance(node, _ast.Name):
        if node.id in _CALC_ALLOWED_NAMES:
            return _CALC_ALLOWED_NAMES[node.id]
        raise _CalcError(f"unknown name '{node.id}'")
    if isinstance(node, _ast.UnaryOp):
        v = _eval_calc_node(node.operand)
        if isinstance(node.op, _ast.USub): return -v
        if isinstance(node.op, _ast.UAdd): return +v
        raise _CalcError(f"unsupported unary op {type(node.op).__name__}")
    if isinstance(node, _ast.BinOp):
        l = _eval_calc_node(node.left)
        r = _eval_calc_node(node.right)
        op = node.op
        if isinstance(op, _ast.Pow):
            try:
                if isinstance(r, (int, float)) and abs(r) > _CALC_MAX_EXPONENT:
                    raise _CalcError(f"exponent magnitude too large (cap {_CALC_MAX_EXPONENT})")
            except OverflowError:
                raise _CalcError(f"exponent magnitude too large (cap {_CALC_MAX_EXPONENT})")
            if isinstance(l, int) and l.bit_length() > _CALC_MAX_BITS:
                raise _CalcError("base magnitude too large")
            res = l ** r
        elif isinstance(op, _ast.Add):      res = l + r
        elif isinstance(op, _ast.Sub):      res = l - r
        elif isinstance(op, _ast.Mult):     res = l * r
        elif isinstance(op, _ast.Div):      res = l / r
        elif isinstance(op, _ast.Mod):      res = l % r
        elif isinstance(op, _ast.FloorDiv): res = l // r
        else:
            raise _CalcError(f"unsupported binary op {type(op).__name__}")
        if isinstance(res, int) and res.bit_length() > _CALC_MAX_BITS:
            raise _CalcError("intermediate result too large")
        return res
    if isinstance(node, _ast.Call):
        if not isinstance(node.func, _ast.Name) or node.func.id not in _CALC_ALLOWED_FUNCS:
            raise _CalcError("unsafe function call")
        fn = _CALC_ALLOWED_FUNCS[node.func.id]
        args = [_eval_calc_node(a) for a in node.args]
        return fn(*args)
    raise _CalcError(f"unsupported node {type(node).__name__}")


def _safe_calc(expr: str) -> str:
    """AST-validated calculator. No eval()/compile() — we evaluate the
    tree ourselves so we can cap intermediate magnitudes between ops."""
    clean = expr.replace("^", "**")
    try:
        tree = _ast.parse(clean, mode="eval")
    except SyntaxError as ex:
        return f"Error: {ex}"
    for node in _ast.walk(tree):
        if not isinstance(node, _CALC_ALLOWED_NODES):
            return f"Error: unsafe expression (disallowed: {type(node).__name__})"
        if isinstance(node, _ast.Constant):
            if not isinstance(node.value, (int, float, complex)):
                return f"Error: only numeric constants allowed"
            try:
                if isinstance(node.value, (int, float)) and abs(node.value) > _CALC_MAX_LITERAL:
                    return f"Error: literal too large"
            except OverflowError:
                return f"Error: literal too large"
        if isinstance(node, _ast.Name) and node.id not in _CALC_ALLOWED_NAMES and node.id not in _CALC_ALLOWED_FUNCS:
            return f"Error: unknown name '{node.id}'"
        if isinstance(node, _ast.Call):
            if not isinstance(node.func, _ast.Name) or node.func.id not in _CALC_ALLOWED_FUNCS:
                return f"Error: unsafe function call"
        # Reject any Pow whose left or right subtree contains another Pow.
        # Catches 9**9**9 (right-nested) AND (999**999)**999 (left-nested).
        if isinstance(node, _ast.BinOp) and isinstance(node.op, _ast.Pow):
            for sub in list(_ast.walk(node.left)) + list(_ast.walk(node.right)):
                if isinstance(sub, _ast.BinOp) and isinstance(sub.op, _ast.Pow):
                    return f"Error: nested ** is not allowed"
    try:
        result = _eval_calc_node(tree.body)
    except _CalcError as ex:
        return f"Error: {ex}"
    except Exception as ex:
        return f"Error: {ex}"
    if isinstance(result, int) and result.bit_length() > _CALC_MAX_BITS:
        return f"Error: result magnitude too large"
    return str(result)


# ==============================================================================
#  SSRF GUARD
# ==============================================================================

def _is_safe_url(url: str) -> Tuple[bool, str]:
    """SSRF protection: reject non-http schemes, private IPs, and metadata endpoints."""
    try:
        parsed = urllib.parse.urlparse(url)
    except Exception:
        return False, "Invalid URL"
    if parsed.scheme not in ("http", "https"):
        return False, f"Blocked scheme: {parsed.scheme}"
    host = parsed.hostname or ""
    if not host:
        return False, "No host"
    blocked_hosts = {"localhost", "metadata.google.internal", "metadata.goog"}
    if host.lower() in blocked_hosts:
        return False, f"Blocked host: {host}"
    # Reject IP-literal hosts that fall inside disallowed ranges before the
    # DNS lookup — covers `http://169.254.169.254/...` directly.
    try:
        import ipaddress
        ip = ipaddress.ip_address(host.strip("[]"))
        if ip.is_loopback or ip.is_private or ip.is_link_local or ip.is_reserved or ip.is_multicast or ip.is_unspecified:
            return False, f"Blocked address: {ip}"
    except ValueError:
        pass  # not an IP literal — fall through to DNS resolution
    try:
        addrs = _socket.getaddrinfo(host, parsed.port or 443, proto=_socket.IPPROTO_TCP)
        import ipaddress
        for family, _, _, _, sockaddr in addrs:
            ip = ipaddress.ip_address(sockaddr[0])
            if ip.is_loopback or ip.is_private or ip.is_link_local or ip.is_reserved or ip.is_multicast or ip.is_unspecified:
                return False, f"Blocked address: {ip}"
    except _socket.gaierror:
        # Fail closed: unresolvable host could be a momentary DNS hiccup or
        # an attacker probing for a name that resolves to a private IP inside
        # httpx (which would then bypass the guard). Refuse rather than allow.
        return False, f"DNS resolution failed for {host}"
    except Exception as ex:
        return False, f"URL safety check failed: {ex}"
    return True, "ok"


# ==============================================================================
#  WORKSPACE PATH RESOLVER
# ==============================================================================

def _resolve_in_workspace(path: str, root: Path, machine_wide: bool = False) -> Path:
    """Resolve a path for a file tool.

    Invariant #7 (CLAUDE.md): both reads AND writes are machine-wide as
    of 2026-05-22 (writes joined reads when the user opted in to broader
    write access). When machine_wide=True, absolute paths anywhere on
    the machine are accepted; relative paths are still anchored to `root`
    for ergonomic continuity, but no containment check fires.

    When machine_wide=False, the legacy sandbox semantics apply: absolute
    paths, parent-directory traversal, and symlinks pointing outside the
    workspace all raise ValueError. This mode is no longer used by any
    Symbion call site but is preserved as a library primitive — flip it
    back on via a cfg toggle if writes ever need re-sandboxing.
    """
    pth = Path(path)
    if machine_wide:
        if pth.is_absolute():
            return pth.resolve()
        return (root / path).resolve()
    resolved_root = root.resolve()
    if pth.is_absolute():
        raise ValueError(f"Path escapes workspace: {path}")
    p = (root / path).resolve()
    try:
        p.relative_to(resolved_root)
    except ValueError:
        raise ValueError(f"Path escapes workspace: {path}")
    if p.is_symlink():
        target = p.resolve()
        try:
            target.relative_to(resolved_root)
        except ValueError:
            raise ValueError(f"Symlink target escapes workspace: {path}")
    return p


# ==============================================================================
#  TOOL SCHEMAS (Anthropic native tool-use format)
# ==============================================================================
#
# Native tool-use schemas (Anthropic format). Used by the agent loop to give
# the responder model the ability to call tools directly during generation.
# Each entry mirrors a SymbionTools method. Keep names in sync with
# SymbionTools._ALLOWED_TOOLS and the dispatch() switch.

TOOL_SCHEMAS = [
    {
        "name": "web_search",
        "description": "Search the web (Brave or DuckDuckGo) for current information. Use for news, prices, releases, recent events, or anything where the answer may have changed since your training.",
        "input_schema": {
            "type": "object",
            "properties": {"query": {"type": "string", "description": "Search query"}},
            "required": ["query"],
        },
    },
    {
        "name": "fetch_url",
        "description": "Fetch and extract readable text from a public HTTPS URL. Use when the user provides a URL or you need a specific page's content.",
        "input_schema": {
            "type": "object",
            "properties": {"url": {"type": "string", "description": "Full http(s) URL"}},
            "required": ["url"],
        },
    },
    {
        "name": "calculate",
        "description": "Evaluate a math expression via AST (safe: no eval). Supports +, -, *, /, **, parens, basic functions.",
        "input_schema": {
            "type": "object",
            "properties": {"expression": {"type": "string", "description": "Math expression like '2+2*3' or '(15+27)/3'"}},
            "required": ["expression"],
        },
    },
    {
        "name": "datetime",
        "description": "Get the current local date and time.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "read_file",
        "description": "Read a text file anywhere on the machine. Path can be absolute (e.g. 'D:/notes/plan.md', 'C:/Users/me/config.json') or relative to workspace root. For PDFs use read_pdf instead. For images use read_image.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path":   {"type": "string", "description": "Absolute or workspace-relative path, e.g. 'symbion_v14.py' or 'D:/Model9/notes.txt'"},
                "offset": {"type": "integer", "description": "Char offset to start reading from", "default": 0},
            },
            "required": ["path"],
        },
    },
    {
        "name": "read_file_chunk",
        "description": "Read a chunk of a large text file at a specific char offset. Use only for multi-MB files where read_file would be too large.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path":      {"type": "string"},
                "offset":    {"type": "integer"},
                "max_chars": {"type": "integer", "default": 2_000_000},
            },
            "required": ["path", "offset"],
        },
    },
    {
        "name": "read_image",
        "description": "Vision-describe an image file (.png/.jpg/.jpeg/.gif/.webp/.bmp) anywhere on the machine. Use this for any image — screenshots, photos, diagrams, charts. Path can be absolute or workspace-relative.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path":   {"type": "string", "description": "Absolute or workspace-relative path to image"},
                "prompt": {"type": "string", "description": "Optional focus, e.g. 'what error is shown?'"},
            },
            "required": ["path"],
        },
    },
    {
        "name": "read_pdf",
        "description": "Extract text from a .pdf file anywhere on the machine via pypdf. Falls back to OCR via pypdfium2 + pytesseract for scanned / image-only PDFs when those deps are installed (plus the Tesseract binary on PATH); otherwise returns a clear 'install pypdfium2 + pytesseract' message rather than confabulating contents. Path can be absolute or workspace-relative.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path":      {"type": "string"},
                "max_chars": {"type": "integer", "default": 50_000},
            },
            "required": ["path"],
        },
    },
    {
        "name": "list_dir",
        "description": "List files and subdirectories of any folder on the machine. Use when the user asks 'what's in folder X', 'list files', or refers to a folder by name without a specific file. Path can be absolute (e.g. 'D:/', 'C:/Users') or workspace-relative. Empty path defaults to workspace root.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path":        {"type": "string", "default": "."},
                "max_entries": {"type": "integer", "default": 200},
            },
        },
    },
    {
        "name": "write_file",
        "description": "Write or overwrite a text file anywhere on the machine. Path can be absolute (e.g. 'D:/notes/out.md', 'C:/Users/me/Desktop/file.txt') or relative to workspace root. USE SPARINGLY — do not write speculatively; only when the user has clearly asked you to create or modify a file.",
        "input_schema": {
            "type": "object",
            "properties": {
                "path":    {"type": "string"},
                "content": {"type": "string"},
            },
            "required": ["path", "content"],
        },
    },
    {
        "name": "get_weather",
        "description": "Current weather at a lat/lon via Open-Meteo (free, no API key). Use when the user asks about weather, temperature, rain, or related conditions for THEIR location — read lat/lon off the 'User's current location' context line and pass them in.",
        "input_schema": {
            "type": "object",
            "properties": {
                "lat": {"type": "number", "description": "Latitude in degrees (-90 to 90)"},
                "lon": {"type": "number", "description": "Longitude in degrees (-180 to 180)"},
            },
            "required": ["lat", "lon"],
        },
    },
    {
        "name": "get_local_time",
        "description": "Current date and time in the user's local timezone. Use when the user asks 'what time is it' or 'is it late here' — read the IANA timezone off the 'User's current location' context line and pass it in. Note: the system prompt's 'Current time' is server-local, which may differ from the user's wall clock when traveling.",
        "input_schema": {
            "type": "object",
            "properties": {
                "timezone": {"type": "string", "description": "IANA timezone (e.g. 'Europe/Madrid', 'America/Los_Angeles')"},
            },
            "required": ["timezone"],
        },
    },
    {
        "name": "get_user_recent_activity",
        "description": "CROSS-USER ONLY. Return ANOTHER household user's recent activity (summaries + message snippets within the last `hours`). NEVER use this for the ACTIVE user — the active user's own recent history is already in your context via build_context. Calling it on yourself is redundant and explicitly wrong; the persona's 'Currently talking to: <name>' line names the active user, and the `user` arg MUST be a different name. Use ONLY when the active user explicitly asks about another known household user by name — 'what was lala working on?', 'is lala still up?', 'did lala mention X?'. Valid names: see the 'Other household users with recent Symbion activity' line in your system prompt. If that line isn't present, no other users have recent activity — don't fabricate names.",
        "input_schema": {
            "type": "object",
            "properties": {
                "user":  {"type": "string", "description": "Name of the OTHER household user to query (e.g. 'lala'). Must match one of the names in cfg.known_users."},
                "hours": {"type": "number", "description": "Look-back window in hours (default 24, capped at 336 / 2 weeks)", "default": 24},
            },
            "required": ["user"],
        },
    },
    {
        "name": "promote_technique",
        "description": (
            "Save a move/technique worth replicating to long-term memory. "
            "Promoted techniques persist verbatim across sessions and (when "
            "shared_learnings sync is configured) across machines — they "
            "surface in future system prompts under 'Techniques worth "
            "replicating' so a future Symbion can replicate the move on a "
            "similar problem.\n\n"
            "USE SPARINGLY. Most turns DO NOT have a move worth promoting. "
            "Promoting your own reasoning should be RARE — one save per "
            "conversation is usually the maximum. If you're not sure, don't "
            "fire this tool.\n\n"
            "Fire ONLY when ALL of the following hold:\n"
            "  - The move is non-obvious. A standard answer to a standard "
            "    question doesn't qualify.\n"
            "  - The move would help another Symbion starting fresh on a "
            "    similar problem (would they NOT have arrived here on their "
            "    own?).\n"
            "  - You can name the move concretely in one sentence — 'reframed "
            "    X as Y which surfaced Z' beats 'thought carefully about X'.\n\n"
            "DON'T fire for: chitchat, factual lookups, simple math, "
            "Q&A where the answer was just retrieval, anything where the "
            "value of your response was in execution rather than approach. "
            "If the user explicitly asks you to save a technique, treat that "
            "as a hint but still apply judgment — they may be overestimating."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "move":  {"type": "string", "description": "The technique in ONE sentence. Specific and concrete. 5-500 chars."},
                "query": {"type": "string", "description": "The user query this move responded to. Quote it directly from the most recent user message in this conversation."},
            },
            "required": ["move", "query"],
        },
    },
]


# ==============================================================================
#  SYMBION TOOLS CLASS
# ==============================================================================

class SymbionTools:
    # Process-wide latch: once Brave returns SUBSCRIPTION_TOKEN_INVALID (or any
    # auth-class 4xx), skip it for the rest of the process so a single agent-loop
    # turn doesn't burn ~2s per call hammering a known-bad key.
    _brave_auth_failed: bool = False

    def __init__(self, workspace_root: str = "./symbion_workspace",
                  memory: Optional["SymbionMemory"] = None):
        self._workspace = Path(workspace_root)
        self._workspace.mkdir(parents=True, exist_ok=True)
        # Memory reference: only the cross-user retrieval tool needs it
        # (get_user_recent_activity). Optional so existing callers that
        # build SymbionTools standalone (tests, helpers) still work; the
        # cross-user tool just returns an error string when memory is
        # missing. SYMBION wires it in at __init__ time.
        self._memory = memory

    @property
    def workspace_path(self) -> Path:
        """Public accessor for the workspace root. Returns the unresolved
        Path so callers can choose to .resolve() it or not."""
        return self._workspace

    @staticmethod
    def calculate(expr: str) -> str:
        return _safe_calc(expr)

    @staticmethod
    def datetime_now() -> str: return datetime.now().strftime("%A, %B %d %Y / %H:%M:%S")

    _IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"}

    def _is_image_path(self, path: str) -> bool:
        return Path(path).suffix.lower() in self._IMAGE_EXTS

    @staticmethod
    def _safe_name(path: str) -> str:
        """Return just the basename for echoing in errors. Keeps absolute paths
        and workspace-relative paths from leaking through error messages into
        logs or the model's context — a low-value info-disclosure vector."""
        try:
            return Path(path).name or "<file>"
        except Exception:
            return "<file>"

    async def read_image(self, path: str, prompt: str, responder, model: str,
                          max_bytes: int = 5_000_000) -> str:
        try:
            if not path.strip(): return "Error: no path given"
            p = _resolve_in_workspace(path.strip(), self._workspace, machine_wide=True)
            name = self._safe_name(path)
            if not p.exists(): return f"Not found: {name}"
            if p.is_dir(): return f"That is a directory, not a file: {name}"
            if not self._is_image_path(str(p)):
                return f"Not a supported image file: {name} (expected png/jpg/gif/webp/bmp)"
            size = p.stat().st_size
            if size > max_bytes:
                return f"Error: image too large ({size} bytes, max {max_bytes})"
            if responder is None or not hasattr(responder, "describe_image"):
                return ("Error: no vision-capable provider configured. "
                        "Image reading needs the Anthropic or OpenAI responder — "
                        "switch with /provider or --provider anthropic.")
            desc = await responder.describe_image(model, str(p), prompt or "")
            return f"[Image description of {p.name} ({size} bytes)]\n{desc}"
        except ValueError as ex:
            return f"Error: invalid image path ({type(ex).__name__}). Report verbatim, do not invent reasons."
        except FileNotFoundError:
            return f"Not found: {self._safe_name(path)}"
        except Exception as ex:
            return f"Error reading image {self._safe_name(path)}: {type(ex).__name__}"

    def read_file(self, path: str, offset: int = 0, max_chars: int = 2_000_000) -> str:
        try:
            if not path.strip(): return "Error: no path given"
            p = _resolve_in_workspace(path.strip(), self._workspace, machine_wide=True)
            name = self._safe_name(path)
            if not p.exists(): return f"Not found: {name}"
            if p.is_dir(): return f"That is a directory, not a file: {name}"
            if self._is_image_path(str(p)):
                return (f"Error: {name} is an image. Use read_image(path) instead — "
                        f"read_file returns raw bytes which are not useful for vision.")
            content = p.read_text(errors="replace")
            total = len(content)
            total_lines = content.count("\n") + (1 if content and not content.endswith("\n") else 0)
            chunk = content[offset:offset + max_chars]
            end = offset + len(chunk)
            remaining = total - end
            # Anchored counts header — keeps the model from eyeballing.
            # 2026-05-24 self-review confab: Symbion read the full file via
            # agent loop (cap-exempted), then asserted "~4500 lines" when
            # the file was 9,303. Putting the truth at the TOP of the
            # response lands ground-truth in working context before the
            # content overwhelms it. Same shape for full and partial reads
            # so callers always see line + char totals.
            if remaining == 0 and offset == 0:
                header = f"[file: {p.name} | {total_lines} lines | {total} chars | full read]\n"
            else:
                header = f"[file: {p.name} | {total_lines} lines | {total} chars | chunk {offset}-{end}]\n"
            suffix = ""
            if remaining > 0:
                suffix = f"\n\n[...{remaining} chars remaining — use read_file_chunk with offset={end} to continue]"
            return header + chunk + suffix
        except ValueError as ex:
            return f"Error: invalid path ({type(ex).__name__}). Report verbatim, do not invent reasons."
        except PermissionError:
            return f"Permission denied reading {self._safe_name(path)} (OS-level ACL). Report verbatim."
        except Exception as ex:
            return f"Error reading {self._safe_name(path)}: {type(ex).__name__}"

    def read_file_chunk(self, path: str, offset: int, max_chars: int = 2_000_000) -> str:
        return self.read_file(path, offset=offset, max_chars=max_chars)

    def list_dir(self, path: str = ".", max_entries: int = 200) -> str:
        try:
            p = _resolve_in_workspace((path or ".").strip(), self._workspace, machine_wide=True)
            if not p.exists():
                return f"Not found: {self._safe_name(path)}"
            if not p.is_dir():
                return f"Not a directory: {self._safe_name(path)}"
            all_entries = sorted(p.iterdir(), key=lambda e: (not e.is_dir(), e.name.lower()))
            shown = all_entries[:max_entries]
            lines = []
            for e in shown:
                if e.is_dir():
                    lines.append(f"[dir]      {e.name}/")
                else:
                    try:
                        size = e.stat().st_size
                        lines.append(f"[{size:>9}b] {e.name}")
                    except OSError:
                        lines.append(f"[?]        {e.name}")
            if len(all_entries) > max_entries:
                lines.append(f"... ({len(all_entries) - max_entries} more entries truncated)")
            header = f"Listing of {p.name or '.'}/ ({len(all_entries)} entries):"
            return header + "\n" + ("\n".join(lines) if lines else "(empty)")
        except ValueError as ex:
            return f"Error: invalid path ({type(ex).__name__}). Report verbatim, do not invent reasons."
        except PermissionError:
            return f"Permission denied listing {self._safe_name(path)} (OS-level ACL). Report verbatim."
        except Exception as ex:
            return f"Error listing {self._safe_name(path)}: {type(ex).__name__}"

    _OCR_MAX_PAGES = 20  # cap rasterisation to keep latency bounded

    def _ocr_pdf(self, p: Path, max_chars: int = 50_000) -> Optional[str]:
        """OCR fallback for scanned PDFs. Returns the extracted text body
        (already wrapped in the standard '[PDF text extracted ...]' header)
        or None if either pypdfium2/pytesseract or the Tesseract binary
        isn't available. Never raises — invariant #2 (graceful degradation)."""
        try:
            import pypdfium2  # type: ignore
            import pytesseract  # type: ignore
        except ImportError:
            return None
        try:
            pdf = pypdfium2.PdfDocument(str(p))
        except Exception as ex:
            logger.warning(f"OCR open {p.name}: {type(ex).__name__}: {ex}")
            return None
        page_count = len(pdf)
        cap = min(page_count, self._OCR_MAX_PAGES)
        parts: List[str] = []
        total = 0
        ok_pages = 0
        try:
            for i in range(cap):
                try:
                    img = pdf[i].render(scale=2).to_pil()
                    txt = pytesseract.image_to_string(img) or ""
                except pytesseract.TesseractNotFoundError:
                    return None
                except Exception as ex:
                    logger.warning(f"OCR page {i+1} of {p.name}: {type(ex).__name__}: {ex}")
                    txt = ""
                if txt.strip():
                    ok_pages += 1
                parts.append(f"--- Page {i+1} (OCR) ---\n{txt}".rstrip())
                total += len(txt)
                if total >= max_chars:
                    parts.append(f"\n[OCR truncated at ~{max_chars} chars]")
                    break
        finally:
            try: pdf.close()
            except Exception: pass
        if ok_pages == 0:
            return None
        suffix = ""
        if cap < page_count:
            suffix = f"\n[OCR'd first {cap} of {page_count} pages — page cap is {self._OCR_MAX_PAGES}]"
        body = "\n\n".join(parts)
        return f"[PDF OCR-extracted from {p.name} ({page_count} pages, {ok_pages} of {cap} OCR'd had text)]\n{body}{suffix}"

    def read_pdf(self, path: str, max_chars: int = 50_000) -> str:
        try:
            if not path.strip(): return "Error: no path given"
            p = _resolve_in_workspace(path.strip(), self._workspace, machine_wide=True)
            name = self._safe_name(path)
            if not p.exists(): return f"Not found: {name}"
            if p.is_dir(): return f"That is a directory, not a file: {name}"
            if p.suffix.lower() != ".pdf":
                return f"Not a PDF: {name} (read_pdf only handles .pdf files)"
            try:
                import pypdf  # type: ignore
            except ImportError:
                return ("Error: pypdf not installed. "
                        "Run `pip install pypdf` in the Symbion environment to enable read_pdf.")
            try:
                reader = pypdf.PdfReader(str(p))
            except Exception as ex:
                return f"Error opening PDF {name}: {type(ex).__name__}: {ex}"
            page_count = len(reader.pages)
            parts: List[str] = []
            total = 0
            non_empty_pages = 0
            for i, page in enumerate(reader.pages):
                try:
                    txt = page.extract_text() or ""
                except Exception:
                    txt = ""
                if txt.strip():
                    non_empty_pages += 1
                parts.append(f"--- Page {i+1} ---\n{txt}".rstrip())
                total += len(txt)
                if total >= max_chars:
                    parts.append(f"\n[truncated at ~{max_chars} chars; PDF has {page_count} pages total]")
                    break
            # If extraction yielded essentially nothing across the whole PDF,
            # try OCR before giving up. pypdf returns nothing on scanned/
            # image-only PDFs because there's no embedded text layer; OCR
            # via pypdfium2 (rasterise) + pytesseract (text extraction)
            # closes that gap.
            if non_empty_pages == 0 and page_count > 0:
                ocr = self._ocr_pdf(p, max_chars=max_chars)
                if ocr is not None:
                    return ocr
                return (f"[PDF {p.name}: {page_count} pages, NO EXTRACTABLE TEXT and no OCR available] "
                        f"This is a scanned/image-only PDF. pypdf cannot read it. "
                        f"To enable OCR fallback, install pypdfium2 + pytesseract and "
                        f"the Tesseract binary (https://github.com/UB-Mannheim/tesseract/wiki). "
                        f"Report this to the user verbatim; do not infer contents from the filename.")
            body = "\n\n".join(parts) if parts else "(empty PDF)"
            return f"[PDF text extracted from {p.name} ({page_count} pages, {non_empty_pages} with text)]\n{body}"
        except ValueError as ex:
            return f"Error: invalid PDF path ({type(ex).__name__}). Report verbatim, do not invent reasons."
        except PermissionError:
            return f"Permission denied reading PDF {self._safe_name(path)} (OS-level ACL). Report verbatim."
        except Exception as ex:
            return f"Error reading PDF {self._safe_name(path)}: {type(ex).__name__}"

    def write_file(self, path: str, content: str) -> str:
        try:
            # Machine-wide writes (2026-05-22): writes joined reads in the
            # machine-wide access pattern. Absolute paths anywhere on the
            # machine are accepted; relative paths anchor to the workspace
            # for ergonomic continuity. Safety is enforced upstream by the
            # judge and persona layers, not by a path sandbox here.
            p = _resolve_in_workspace(path.strip(), self._workspace,
                                       machine_wide=True)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content, encoding='utf-8')
            return f"Written {len(content)} chars to {p}"
        except Exception as ex:
            return f"Error writing {self._safe_name(path)}: {type(ex).__name__}"

    @classmethod
    async def web_search(cls, query: str, brave_key: str = "", max_chars: int = 2400) -> str:
        # Per-backend failure reasons, surfaced in the final error so the
        # responder can tell the user the truth rather than "no results."
        reasons: List[str] = []
        if not _HTTPX:
            return ("Search unavailable for: " + query +
                    " | reasons: httpx not installed | suggest: pip install httpx")

        if brave_key and not cls._brave_auth_failed:
            try:
                async with httpx.AsyncClient(timeout=8) as c:
                    r = await c.get(
                        "https://api.search.brave.com/res/v1/web/search",
                        params={"q": query, "count": 5},
                        headers={"Accept": "application/json",
                                 "X-Subscription-Token": brave_key})
                if r.status_code == 200:
                    data = r.json()
                    parts = [f"{rr.get('title','')}: {rr.get('description','')} ({rr.get('url','')})"
                             for rr in data.get("web",{}).get("results",[])[:4] if rr.get("description")]
                    if parts: return "\n".join(parts)[:max_chars]
                    reasons.append("Brave returned no usable results")
                else:
                    # Latch auth failures so the rest of the process skips Brave
                    # instead of paying ~2s per call to learn the same thing.
                    # Brave returns 422 (not 401) for an invalid token, with the
                    # specific error code in the JSON body — so we have to read
                    # enough of the body to find it.
                    body = r.text[:600] if r.text else ""
                    auth_marker = ("SUBSCRIPTION_TOKEN_INVALID" in body
                                   or '"component":"authentication"' in body)
                    if r.status_code in (401, 403) or auth_marker:
                        cls._brave_auth_failed = True
                        reasons.append("Brave key rejected (regenerate at brave.com/search/api)")
                        logger.warning(f"Brave auth failed; disabling for this process.")
                    else:
                        reasons.append(f"Brave HTTP {r.status_code}")
                        logger.warning(f"Brave: HTTP {r.status_code}: {body[:300]}")
            except Exception as ex:
                reasons.append(f"Brave error: {type(ex).__name__}")
                logger.warning(f"Brave: {ex}")
        elif cls._brave_auth_failed:
            reasons.append("Brave skipped (key previously rejected)")

        try:
            async with httpx.AsyncClient(timeout=10) as c:
                r = await c.get(
                    "https://html.duckduckgo.com/html/",
                    params={"q": query, "df": "w"},
                    headers={"User-Agent":"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"})
            html = r.text
            # DDG serves an anomaly/captcha page to scripted clients. The page
            # has no real result classes, just `anomaly-modal__*`. Without this
            # check the regex returns [] and the function lies "no results."
            if "anomaly-modal" in html or "anomaly_modal" in html:
                reasons.append("DDG bot-blocked (anomaly page)")
                logger.warning("DDG: anomaly page served")
            else:
                snippets = re.findall(r'class="result__snippet"[^>]*>(.*?)</a>',html,re.DOTALL)
                titles   = re.findall(r'class="result__a"[^>]*>(.*?)</a>',html,re.DOTALL)
                urls     = re.findall(r'class="result__url"[^>]*>(.*?)</span>',html,re.DOTALL)
                parts = []
                for t,s,u in zip(titles[:6],snippets[:6],urls[:6]+[""]*6):
                    t2 = re.sub('<[^>]+>','',t).strip()
                    s2 = re.sub('<[^>]+>','',s).strip()
                    u2 = re.sub('<[^>]+>','',u).strip()
                    if t2 and s2: parts.append(f"{t2}: {s2}" + (f" [{u2}]" if u2 else ""))
                if parts: return "\n".join(parts)[:max_chars]
                reasons.append("DDG HTML parse returned nothing (selectors may have changed)")
        except Exception as ex:
            reasons.append(f"DDG error: {type(ex).__name__}")
            logger.warning(f"DDG: {ex}")

        try:
            async with httpx.AsyncClient(timeout=6) as c:
                r = await c.get(
                    "https://api.duckduckgo.com/",
                    params={"q": query, "format": "json",
                            "no_html": "1", "skip_disambig": "1"},
                    headers={"User-Agent":"Symbion/14.0"})
            data = r.json()
            parts = ([data["AbstractText"]] if data.get("AbstractText") else []) + \
                    [t["Text"] for t in data.get("RelatedTopics",[])[:3]
                     if isinstance(t,dict) and t.get("Text")]
            if parts: return " ".join(parts)[:max_chars]
            reasons.append("DDG Instant Answer empty (no definitional match)")
        except Exception as ex:
            reasons.append(f"Instant Answer error: {type(ex).__name__}")

        # All three backends down. Return an honest, actionable error string —
        # the responder is instructed not to confabulate when a tool says it
        # failed, so this prevents "I searched and found nothing" answers.
        return ("Search unavailable for: " + query +
                " | reasons: " + "; ".join(reasons) +
                " | suggest: refresh BRAVE_API_KEY or use fetch_url against a specific source")

    # Hosts that ship empty-shell HTML and require JS to render content.
    # Plain urllib gets a useless skeleton from these — skip the direct
    # fetch and go straight to Jina Reader (which renders JS server-side).
    _JS_GATED_HOSTS = frozenset({
        "x.com", "twitter.com", "mobile.twitter.com",
        "instagram.com", "www.instagram.com",
        "linkedin.com", "www.linkedin.com",
        "facebook.com", "www.facebook.com", "m.facebook.com",
        "tiktok.com", "www.tiktok.com",
        "threads.net", "www.threads.net",
    })
    # Markers in returned text that indicate we hit a JS-required wall,
    # CDN challenge page, or login redirect rather than real content.
    _JS_GATE_MARKERS = (
        "javascript is required", "enable javascript", "please enable javascript",
        "checking your browser", "just a moment", "ddos protection by",
        "you need to enable javascript to run this app",
        "this content isn't available", "log in to view",
    )
    _MIN_USEFUL_CHARS = 200  # below this, treat as failed render

    @classmethod
    def _looks_js_gated(cls, url: str, text: str) -> bool:
        """True when the response body itself reveals a JS-required wall
        or CDN challenge. Length alone is NOT a trigger — many legitimate
        small pages exist (example.com is ~155 chars). The host-prelist
        in fetch_url handles known-bad domains separately."""
        try:
            host = urllib.parse.urlparse(url).hostname or ""
        except Exception:
            host = ""
        if host.lower() in cls._JS_GATED_HOSTS:
            return True
        low = text[:2000].lower()
        return any(m in low for m in cls._JS_GATE_MARKERS)

    @staticmethod
    async def _fetch_via_jina(url: str, max_chars: int) -> Optional[str]:
        """Retry fetch through r.jina.ai — Jina's free Reader endpoint
        renders JS server-side and returns clean markdown. No API key
        needed for low-volume use. Returns None on any failure so caller
        can fall back to a clear error message rather than crashing."""
        if not _HTTPX:
            return None
        try:
            async with httpx.AsyncClient(timeout=20, follow_redirects=True) as c:
                r = await c.get(
                    "https://r.jina.ai/" + url,
                    headers={"User-Agent":"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
                             "Accept": "text/plain"})
            text = r.text
            text = re.sub(r'\s+', ' ', text).strip()
            if len(text) < SymbionTools._MIN_USEFUL_CHARS:
                return None
            head = text[:max_chars]
            tail = f"\n[...truncated via r.jina.ai, fetched {len(text)} chars total]" if len(text) > max_chars else "\n[fetched via r.jina.ai — JS-rendered]"
            return head + tail
        except Exception as ex:
            logger.warning(f"Jina fallback failed for {url}: {type(ex).__name__}: {ex}")
            return None

    @classmethod
    async def fetch_url(cls, url: str, max_chars: int = 4000) -> str:
        safe, reason = _is_safe_url(url)
        if not safe:
            return f"Error: blocked URL — {reason}"

        try:
            host = (urllib.parse.urlparse(url).hostname or "").lower()
        except Exception:
            host = ""

        # Skip the direct fetch entirely for hosts known to ship empty-shell
        # HTML — saves a wasted roundtrip and the misleading "got nothing"
        # state.
        if host in cls._JS_GATED_HOSTS:
            jina = await cls._fetch_via_jina(url, max_chars)
            if jina is not None:
                return jina
            return (f"Error fetching {url}: {host} requires JavaScript and the "
                    f"r.jina.ai render fallback also failed. Report this to the "
                    f"user verbatim — do not invent the page contents.")

        if not _HTTPX:
            return f"Error fetching {url}: httpx not installed"
        try:
            # Manual redirect walk so _is_safe_url runs against every hop.
            # follow_redirects=True would let a public URL bounce to
            # 127.0.0.1, AWS metadata, etc. after the initial check.
            current = url
            hops = 0
            MAX_HOPS = 5
            async with httpx.AsyncClient(timeout=12, follow_redirects=False) as c:
                while True:
                    r = await c.get(
                        current,
                        headers={"User-Agent":"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"})
                    if r.status_code in (301, 302, 303, 307, 308):
                        loc = r.headers.get("location") or r.headers.get("Location")
                        if not loc:
                            break
                        nxt = urllib.parse.urljoin(current, loc)
                        hops += 1
                        if hops > MAX_HOPS:
                            return f"Error fetching {url}: too many redirects (> {MAX_HOPS})"
                        safe_n, reason_n = _is_safe_url(nxt)
                        if not safe_n:
                            return f"Error: blocked redirect target — {reason_n}"
                        current = nxt
                        continue
                    break
            html = r.text
            html = re.sub(r'<script[^>]*>.*?</script>','',html,flags=re.DOTALL|re.IGNORECASE)
            html = re.sub(r'<style[^>]*>.*?</style>','',html,flags=re.DOTALL|re.IGNORECASE)
            text = re.sub(r'<[^>]+>','',html)
            text = re.sub(r'\s+',' ',text).strip()

            # Detect JS-gated / challenge-page failures the response body
            # itself reveals (cloudflare interstitial, "enable JS" stubs,
            # near-empty SPAs we didn't pre-list above). Retry via Jina
            # Reader which renders client-side JS for us.
            if cls._looks_js_gated(url, text):
                jina = await cls._fetch_via_jina(url, max_chars)
                if jina is not None:
                    return jina
                return (f"Error fetching {url}: page showed a JS-required or CDN "
                        f"challenge marker, and the r.jina.ai render fallback also "
                        f"failed. The site likely requires JavaScript. Report this "
                        f"verbatim — do not invent the page contents.")

            return text[:max_chars] + (f"\n[...truncated, fetched {len(text)} chars total]" if len(text)>max_chars else "")
        except Exception as ex:
            # Even on outright HTTP failure, give Jina a shot — many 403s
            # come from servers that refuse non-browser UAs but Jina handles.
            jina = await cls._fetch_via_jina(url, max_chars)
            if jina is not None:
                return jina
            return f"Error fetching {url}: {ex}"

    # ---- Location-aware tools ----
    # Both consume the location surfaced in build_context: the model
    # reads lat/lon (or timezone) off the system-prompt context line
    # and passes them in. No memory plumbing through SymbionTools.

    _WEATHER_CODE = {
        0: "clear sky", 1: "mainly clear", 2: "partly cloudy", 3: "overcast",
        45: "fog", 48: "depositing rime fog",
        51: "light drizzle", 53: "drizzle", 55: "dense drizzle",
        56: "light freezing drizzle", 57: "freezing drizzle",
        61: "light rain", 63: "rain", 65: "heavy rain",
        66: "light freezing rain", 67: "freezing rain",
        71: "light snow", 73: "snow", 75: "heavy snow", 77: "snow grains",
        80: "light rain showers", 81: "rain showers", 82: "violent rain showers",
        85: "light snow showers", 86: "snow showers",
        95: "thunderstorm", 96: "thunderstorm with light hail",
        99: "thunderstorm with heavy hail",
    }

    async def get_weather(self, lat: float, lon: float) -> str:
        """Open-Meteo current weather (free, no API key). Returns a short
        natural-language summary so the model can quote it directly."""
        if not _HTTPX:
            return "Error: httpx not installed; cannot fetch weather"
        try:
            lat = float(lat); lon = float(lon)
        except (TypeError, ValueError):
            return "Error: lat and lon must be numbers"
        if not (-90 <= lat <= 90 and -180 <= lon <= 180):
            return "Error: lat/lon out of range"
        try:
            async with httpx.AsyncClient(timeout=10.0) as cli:
                r = await cli.get(
                    "https://api.open-meteo.com/v1/forecast",
                    params={
                        "latitude": lat, "longitude": lon,
                        "current": "temperature_2m,relative_humidity_2m,"
                                   "apparent_temperature,is_day,precipitation,"
                                   "weather_code,wind_speed_10m",
                        "timezone": "auto",
                    })
            if r.status_code != 200:
                return f"Error fetching weather: HTTP {r.status_code}"
            data = r.json()
            cur = data.get("current") or {}
            if not cur:
                return "Error: weather response missing 'current' block"
            code = int(cur.get("weather_code", -1))
            desc = self._WEATHER_CODE.get(code, f"unknown weather code {code}")
            temp = cur.get("temperature_2m")
            feels = cur.get("apparent_temperature")
            precip = cur.get("precipitation")
            wind = cur.get("wind_speed_10m")
            hum = cur.get("relative_humidity_2m")
            lines = [f"Currently {desc}."]
            if temp is not None:
                feels_part = f" (feels {feels:g}°C)" if feels is not None else ""
                lines.append(f"Temperature: {temp:g}°C{feels_part}.")
            if hum is not None:
                lines.append(f"Humidity: {hum:g}%.")
            if precip is not None and float(precip) > 0:
                lines.append(f"Precipitation: {precip:g} mm in the last hour.")
            if wind is not None:
                lines.append(f"Wind: {wind:g} km/h.")
            return " ".join(lines)
        except Exception as ex:
            return f"Error fetching weather: {type(ex).__name__}: {ex}"

    def get_local_time(self, timezone: str) -> str:
        """Current time in the given IANA timezone. The model reads the
        user's timezone off the context line and passes it here when the
        question is locally-anchored ('what time is it', 'is it late')."""
        try:
            from zoneinfo import ZoneInfo
            zi = ZoneInfo(str(timezone))
        except Exception:
            return f"Error: unknown timezone '{timezone}' (expected IANA name like Europe/Madrid)"
        now = datetime.now(zi)
        return now.strftime("%A, %B %d %Y, %I:%M %p %Z").lstrip("0")

    def get_user_recent_activity(self, target_user: str, cfg: "SymbionConfig",
                                  hours: float = 24.0,
                                  active_user: str = "") -> str:
        """Cross-user retrieval (Phase 2). When ONE household member asks
        Symbion about another ('what was lala working on', 'is lala
        still up?'), this tool returns a formatted snapshot of the
        target user's recent summaries + a few raw message snippets.
        Validated against cfg.known_users so a malicious / confused
        call can't leak arbitrary names.

        Symmetric: any known user can query any other known user.
        Returns a single string so the model can quote it directly."""
        if self._memory is None:
            return "Error: memory not wired to tools layer"
        target = (target_user or "").strip().lower()
        known = [u.lower() for u in (cfg.known_users or [])]
        if not target or target not in known:
            return (f"Error: unknown user '{target_user}'. Known users in "
                    f"this household: {', '.join(cfg.known_users or [])}")
        # Refuse self-queries: the active user's own recent history is
        # already in the build_context recent + summary blocks. The tool
        # exists specifically to fetch OTHER household users' activity.
        # The caller MUST pass active_user (resolved per-session via
        # SYMBION._active_user(session)). Falling back to cfg.active_user
        # was wrong: a session where /user switched to lala but cfg.active_user
        # is still "aaron" would let a Lala-querying-Lala call slip past.
        # Fail closed when active_user is missing so unthreaded paths are
        # caught loud instead of letting the wrong user's data through.
        active = (active_user or "").strip().lower()
        if not active:
            return ("Error: get_user_recent_activity needs session context. "
                    "Caller must thread the per-session active user (via "
                    "SYMBION._active_user) into tools.dispatch.")
        if target == active:
            return (f"Error: get_user_recent_activity is for OTHER household "
                    f"members. '{target}' is the active user — their recent "
                    f"history is already in your system-prompt context. "
                    f"Just answer from that.")
        try:
            hours_f = float(hours)
        except (TypeError, ValueError):
            hours_f = 24.0
        hours_f = max(0.1, min(hours_f, 24.0 * 14))  # cap at 2 weeks
        data = self._memory.get_user_recent_activity(target, hours=hours_f)
        out = [f"{target} — last active: {data['last_active_ago']}"]
        if data["summaries"]:
            out.append("\nRecent summaries:")
            for s in data["summaries"]:
                ts = (s.get("ts") or "")[:16].replace("T", " ")
                out.append(f"  [{ts}] {s['content']}")
        if data["messages"]:
            out.append("\nRecent message snippets:")
            for m in data["messages"]:
                ts = (m.get("ts") or "")[:16].replace("T", " ")
                role = "you (sym)" if m["role"] == "assistant" else target
                snippet = (m["content"] or "").replace("\n", " ")[:140]
                out.append(f"  [{ts}] {role}: {snippet}")
        if not data["summaries"] and not data["messages"]:
            out.append("(No content in the last "
                       f"{int(hours_f) if hours_f>=1 else hours_f}h.)")
        return "\n".join(out)

    _ALLOWED_TOOLS = frozenset({
        "calculate","datetime","read_file","read_file_chunk",
        "read_image","read_pdf","list_dir",
        "write_file","web_search","fetch_url",
        "get_weather","get_local_time",
        "get_user_recent_activity",
        "promote_technique",
    })
    _MAX_PATH_LEN = 1024
    _MAX_URL_LEN = 2048
    _MAX_QUERY_LEN = 2000
    _MAX_EXPR_LEN = 500
    _MAX_CONTENT_LEN = 5_000_000
    _MAX_PROMPT_LEN = 1000
    _PATH_BAD_CHARS = re.compile(r'[\x00-\x1f]')

    @classmethod
    def _validate_args(cls, tool: str, args: Dict) -> Tuple[bool, str, Dict]:
        """Sanity-check LLM-supplied tool args before any filesystem/network I/O.

        Ensures args is a dict with expected string/int shapes, caps lengths,
        and rejects null bytes / control chars in paths. Returns
        (ok, error_message, normalized_args). Downstream validators (the
        workspace sandbox, SSRF check, AST calculator) still run — this is
        the outer perimeter, not the only line of defence.
        """
        if tool not in cls._ALLOWED_TOOLS:
            return False, f"Unknown tool: {tool}", {}
        if not isinstance(args, dict):
            return False, "tool_args must be an object", {}

        out: Dict = {}

        def _str(key: str, max_len: int, required: bool = True) -> Optional[str]:
            v = args.get(key, "")
            if not isinstance(v, str):
                return None
            v = v.strip()
            if required and not v:
                return None
            if len(v) > max_len:
                v = v[:max_len]
            return v

        def _int(key: str, default: int = 0, lo: int = 0, hi: int = 10**9) -> Optional[int]:
            v = args.get(key, default)
            try:
                iv = int(v)
            except (TypeError, ValueError):
                return None
            if iv < lo or iv > hi:
                return None
            return iv

        def _check_path(p: str) -> Tuple[bool, str]:
            if cls._PATH_BAD_CHARS.search(p):
                return False, "path contains control characters"
            return True, ""

        if tool == "calculate":
            expr = _str("expression", cls._MAX_EXPR_LEN)
            if expr is None: return False, "calculate requires string expression", {}
            out["expression"] = expr
        elif tool == "datetime":
            pass
        elif tool in ("read_file", "read_file_chunk", "read_image", "read_pdf"):
            path = _str("path", cls._MAX_PATH_LEN)
            if path is None: return False, f"{tool} requires string path", {}
            ok, reason = _check_path(path)
            if not ok: return False, reason, {}
            out["path"] = path
            offset = _int("offset", default=0, lo=0, hi=cls._MAX_CONTENT_LEN)
            if offset is None: return False, "offset must be a non-negative integer", {}
            out["offset"] = offset
            if tool == "read_file_chunk":
                mc = _int("max_chars", default=2_000_000, lo=1, hi=cls._MAX_CONTENT_LEN)
                if mc is None: return False, "max_chars must be a positive integer", {}
                out["max_chars"] = mc
            if tool == "read_image":
                prompt = _str("prompt", cls._MAX_PROMPT_LEN, required=False) or ""
                out["prompt"] = prompt
            if tool == "read_pdf":
                mc = _int("max_chars", default=50_000, lo=1, hi=cls._MAX_CONTENT_LEN)
                if mc is None: return False, "max_chars must be a positive integer", {}
                out["max_chars"] = mc
        elif tool == "list_dir":
            # path is optional — defaults to "." (workspace root) if missing.
            path = _str("path", cls._MAX_PATH_LEN, required=False) or "."
            ok, reason = _check_path(path)
            if not ok: return False, reason, {}
            out["path"] = path
            mx = _int("max_entries", default=200, lo=1, hi=10_000)
            if mx is None: return False, "max_entries must be a positive integer", {}
            out["max_entries"] = mx
        elif tool == "write_file":
            path = _str("path", cls._MAX_PATH_LEN)
            if path is None: return False, "write_file requires string path", {}
            ok, reason = _check_path(path)
            if not ok: return False, reason, {}
            content = args.get("content", "")
            if not isinstance(content, str):
                return False, "write_file content must be a string", {}
            if len(content) > cls._MAX_CONTENT_LEN:
                return False, f"write_file content exceeds {cls._MAX_CONTENT_LEN} chars", {}
            out["path"] = path
            out["content"] = content
        elif tool == "web_search":
            q = _str("query", cls._MAX_QUERY_LEN)
            if q is None: return False, "web_search requires string query", {}
            out["query"] = q
        elif tool == "fetch_url":
            url = _str("url", cls._MAX_URL_LEN)
            if url is None: return False, "fetch_url requires string url", {}
            out["url"] = url
        elif tool == "get_weather":
            for key in ("lat", "lon"):
                v = args.get(key)
                try:
                    fv = float(v)
                except (TypeError, ValueError):
                    return False, f"get_weather requires numeric {key}", {}
                out[key] = fv
            if not (-90 <= out["lat"] <= 90 and -180 <= out["lon"] <= 180):
                return False, "lat/lon out of range", {}
        elif tool == "get_local_time":
            tz = _str("timezone", 64)
            if tz is None: return False, "get_local_time requires IANA timezone string", {}
            out["timezone"] = tz
        elif tool == "get_user_recent_activity":
            target = _str("user", 32)
            if target is None: return False, "get_user_recent_activity requires 'user' string", {}
            out["user"] = target
            hrs = args.get("hours", 24)
            try:
                out["hours"] = float(hrs)
            except (TypeError, ValueError):
                out["hours"] = 24.0
        elif tool == "promote_technique":
            mv = _str("move", 500)
            if mv is None or len(mv) < 5:
                return False, "promote_technique requires a 'move' string (5-500 chars)", {}
            out["move"] = mv
            qv = _str("query", 1000, required=False)
            out["query"] = qv or ""

        return True, "", out

    def promote_technique(self, move: str, query: str,
                            session: str = "",
                            user: str = "aaron") -> str:
        """Save a model-promoted technique to long-term memory. Called via
        the agent loop when the model decides a move is worth replicating.
        Unlike user-initiated /promote, this path doesn't ask the judge
        for extraction — the model produces the move text directly. Both
        paths land in the same techniques table (source='local')."""
        if self._memory is None:
            return "Error: memory not wired to tools layer"
        try:
            tid = self._memory.save_technique(
                query=query or "(model-promoted, query not captured)",
                move=move,
                evidence="",
                session=session,
                user=user or "aaron",
                embedding=None,
                source="local")
        except Exception as ex:
            return f"Error: technique save failed: {type(ex).__name__}: {ex}"
        return (f"Technique #{tid} saved. Use this SPARINGLY — one save per "
                f"conversation is usually the max; most turns don't have a "
                f"move worth preserving.")

    async def dispatch(self, tool: str, args: Dict, cfg: "SymbionConfig",
                       responder=None, responder_model: str = "",
                       active_user: str = "",
                       session: str = "") -> str:
        ok, reason, a = self._validate_args(tool, args)
        if not ok:
            logger.warning(f"Tool arg validation failed: {tool} — {reason}")
            return f"Error: {reason}"
        if tool=="calculate":       return self.calculate(a["expression"])
        if tool=="datetime":        return self.datetime_now()
        if tool=="read_file":       return self.read_file(a["path"], a.get("offset",0))
        if tool=="read_file_chunk": return self.read_file_chunk(a["path"], a.get("offset",0), a.get("max_chars",2_000_000))
        if tool=="read_image":      return await self.read_image(a["path"], a.get("prompt",""), responder, responder_model)
        if tool=="read_pdf":        return self.read_pdf(a["path"], a.get("max_chars",50_000))
        if tool=="list_dir":        return self.list_dir(a.get("path","."), a.get("max_entries",200))
        if tool=="write_file":      return self.write_file(a["path"], a["content"])
        if tool=="web_search":      return await self.web_search(a["query"], cfg.brave_api_key, cfg.search_max_chars)
        if tool=="fetch_url":       return await self.fetch_url(a["url"], cfg.search_max_chars)
        if tool=="get_weather":     return await self.get_weather(a["lat"], a["lon"])
        if tool=="get_local_time":  return self.get_local_time(a["timezone"])
        if tool=="get_user_recent_activity":
            return self.get_user_recent_activity(a["user"], cfg, a.get("hours", 24.0),
                                                  active_user=active_user)
        if tool=="promote_technique":
            return self.promote_technique(a["move"], a["query"],
                                            session=session, user=active_user or "aaron")
        return f"Unknown tool: {tool}"
