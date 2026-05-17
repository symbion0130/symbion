"""
Symbion OneDrive sync.

Carries conversation state (symbion.db + telemetry logs) between machines
via a shared OneDrive folder. Pull-on-start / push-on-clean-exit pattern so
the SQLite DB is never live-synced while open (which corrupts it).

OneDrive layout (%OneDrive%/Symbion/sync/):
    symbion.db                 SQLite store (conversations, summaries, identity)
    symbion_events.jsonl       append-only telemetry
    symbion_transparency.log   append-only audit log
    session.lock               JSON {machine, started, pid} — present iff a
                               machine claims an active session

NOT synced (per-machine, by design):
    .env (API keys), symbion.json (config), .python/ (portable interpreter),
    archive/, *.bak.*

Usage (run from repo root or via wrapper):
    python scripts/sync.py pull            pull state from OneDrive, acquire lock
    python scripts/sync.py push            push state back, release lock
    python scripts/sync.py status          show what's where
    python scripts/sync.py pull --force    take over a lock held by another machine
"""
import argparse
import json
import os
import platform
import shutil
import socket
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

SYNCED_FILES = ["symbion.db", "symbion_events.jsonl", "symbion_transparency.log"]
LOCK_NAME = "session.lock"
STALE_LOCK_HOURS = 24
MAX_BACKUPS = 5

REPO = Path(__file__).resolve().parent.parent
_ONEDRIVE_RAW = os.environ.get("OneDrive") or os.environ.get("OneDriveConsumer") or ""
ONEDRIVE = Path(_ONEDRIVE_RAW) if _ONEDRIVE_RAW else None
SYNC_DIR = (ONEDRIVE / "Symbion" / "sync") if ONEDRIVE else None


def _info(msg: str) -> None:
    print(f"[sync] {msg}")


def _err(msg: str, code: int = 1) -> None:
    print(f"[sync] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def _ts() -> str:
    return datetime.now(timezone.utc).isoformat()


def _machine() -> str:
    return socket.gethostname() or platform.node() or "unknown"


def _require_onedrive() -> None:
    if ONEDRIVE is None or not ONEDRIVE.exists():
        _err(
            f"OneDrive folder not found (env OneDrive={_ONEDRIVE_RAW!r}). "
            "Open OneDrive once so the folder exists, then retry."
        )
    SYNC_DIR.mkdir(parents=True, exist_ok=True)


def _check_local_db_not_busy() -> None:
    """Refuse if another process holds the local symbion.db."""
    db = REPO / "symbion.db"
    if not db.exists():
        return
    try:
        con = sqlite3.connect(f"file:{db}?mode=rw", uri=True, timeout=2.0)
        con.execute("BEGIN IMMEDIATE")
        con.rollback()
        con.close()
    except sqlite3.OperationalError as e:
        _err(
            f"Local symbion.db is busy ({e}). Close the running Symbion "
            "instance on this machine, then retry."
        )


def _checkpoint_wal(db: Path) -> None:
    """Fold WAL into main DB so copying just symbion.db captures all writes.

    Symbion runs SQLite in WAL mode, so recent writes can sit in symbion.db-wal
    instead of the main file. Without this, push would copy a stale .db and the
    other machine would never see the tail of the last session.
    """
    if not db.exists():
        return
    try:
        con = sqlite3.connect(f"file:{db}?mode=rw", uri=True, timeout=5.0)
        con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        con.close()
    except sqlite3.OperationalError as e:
        _info(f"{db.name}: WAL checkpoint skipped ({e})")


def _read_lock() -> dict | None:
    p = SYNC_DIR / LOCK_NAME
    if not p.exists():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return {"machine": "?", "started": "?", "pid": None, "_corrupt": True}


def _write_lock() -> None:
    p = SYNC_DIR / LOCK_NAME
    payload = {"machine": _machine(), "started": _ts(), "pid": os.getpid()}
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(payload), encoding="utf-8")
    os.replace(tmp, p)


def _clear_lock(force: bool = False) -> None:
    p = SYNC_DIR / LOCK_NAME
    if not p.exists():
        return
    if not force:
        lock = _read_lock()
        if lock and lock.get("machine") != _machine():
            _info(f"not clearing lock owned by {lock.get('machine')}")
            return
    try:
        p.unlink()
    except OSError:
        pass


def _atomic_copy(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(dst.suffix + ".tmp")
    shutil.copy2(src, tmp)
    os.replace(tmp, dst)


def _backup_local(name: str) -> Path | None:
    src = REPO / name
    if not src.exists():
        return None
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    dst = REPO / f"{name}.bak.{stamp}"
    shutil.copy2(src, dst)
    backups = sorted(
        REPO.glob(f"{name}.bak.*"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    for old in backups[MAX_BACKUPS:]:
        try:
            old.unlink()
        except OSError:
            pass
    return dst


def _mtime(p: Path) -> float:
    try:
        return p.stat().st_mtime
    except OSError:
        return 0.0


def _lock_age_hours(lock: dict) -> float:
    started = lock.get("started", "")
    try:
        t0 = datetime.fromisoformat(started.replace("Z", "+00:00"))
        if t0.tzinfo is None:
            t0 = t0.replace(tzinfo=timezone.utc)
        return (datetime.now(timezone.utc) - t0).total_seconds() / 3600.0
    except Exception:
        return 9999.0


def cmd_pull(args) -> int:
    _require_onedrive()
    _check_local_db_not_busy()

    lock = _read_lock()
    if lock:
        owner = lock.get("machine")
        started = lock.get("started", "?")
        if owner == _machine():
            _info(f"reclaiming our own lock (started {started})")
        else:
            age = _lock_age_hours(lock)
            if age < STALE_LOCK_HOURS and not args.force:
                _err(
                    f"Symbion session active on '{owner}' (started {started}, "
                    f"{age:.1f}h ago). If that machine exited uncleanly, "
                    "re-run via `python scripts\\sync.py pull --force` to take "
                    "over (you may lose any unpushed work from that machine)."
                )
            _info(f"taking over lock from '{owner}' (age {age:.1f}h, forced={args.force})")

    for name in SYNCED_FILES:
        local = REPO / name
        remote = SYNC_DIR / name
        if not remote.exists() and not local.exists():
            continue
        if not remote.exists():
            _info(f"{name}: no remote copy yet (will create on push)")
            continue
        if not local.exists():
            _atomic_copy(remote, local)
            _info(f"{name}: pulled {local.stat().st_size:,}B (no local copy)")
            continue
        rt = _mtime(remote)
        lt = _mtime(local)
        if rt > lt + 1.0:
            if name == "symbion.db":
                bak = _backup_local(name)
                if bak:
                    _info(f"{name}: backed up local to {bak.name}")
            _atomic_copy(remote, local)
            _info(f"{name}: pulled (remote newer by {rt - lt:.0f}s)")
        elif lt > rt + 1.0:
            _info(
                f"{name}: keeping local (newer by {lt - rt:.0f}s — "
                "previous push likely failed; will push on next clean exit)"
            )
        else:
            _info(f"{name}: in sync")

    _write_lock()
    _info(f"lock acquired by '{_machine()}'")
    return 0


def cmd_push(args) -> int:
    if ONEDRIVE is None or not ONEDRIVE.exists():
        _info("OneDrive not available — skipping push")
        return 0
    if SYNC_DIR is None:
        return 0
    SYNC_DIR.mkdir(parents=True, exist_ok=True)

    for name in SYNCED_FILES:
        local = REPO / name
        if not local.exists():
            continue
        if name == "symbion.db":
            _checkpoint_wal(local)
        remote = SYNC_DIR / name
        try:
            _atomic_copy(local, remote)
            _info(f"{name}: pushed {local.stat().st_size:,}B")
        except Exception as e:
            _info(f"{name}: push FAILED ({e})")

    _clear_lock()
    _info("lock released")
    return 0


def cmd_status(args) -> int:
    print(f"machine     : {_machine()}")
    print(f"repo        : {REPO}")
    print(f"OneDrive    : {ONEDRIVE if ONEDRIVE else '(not detected)'}")
    print(f"sync dir    : {SYNC_DIR if SYNC_DIR else '(n/a)'}")
    if SYNC_DIR and SYNC_DIR.exists():
        lock = _read_lock()
        if lock:
            age = _lock_age_hours(lock)
            print(f"LOCK        : '{lock.get('machine')}' since {lock.get('started')} ({age:.1f}h ago)")
        else:
            print("LOCK        : (none)")
        print()
        for name in SYNCED_FILES:
            local = REPO / name
            remote = SYNC_DIR / name
            l_exists = local.exists()
            r_exists = remote.exists()
            l_size = local.stat().st_size if l_exists else 0
            r_size = remote.stat().st_size if r_exists else 0
            l_mt = (
                datetime.fromtimestamp(_mtime(local)).isoformat(timespec="seconds")
                if l_exists else "-"
            )
            r_mt = (
                datetime.fromtimestamp(_mtime(remote)).isoformat(timespec="seconds")
                if r_exists else "-"
            )
            verdict = "in sync"
            if not l_exists and r_exists:
                verdict = "REMOTE ONLY"
            elif l_exists and not r_exists:
                verdict = "LOCAL ONLY"
            elif l_exists and r_exists:
                if _mtime(remote) > _mtime(local) + 1.0:
                    verdict = "remote newer"
                elif _mtime(local) > _mtime(remote) + 1.0:
                    verdict = "local newer"
            print(f"  {name}  [{verdict}]")
            print(f"    local : {l_size:>12,}B  {l_mt}")
            print(f"    remote: {r_size:>12,}B  {r_mt}")
    else:
        print("sync dir does not exist yet (run `pull` or `push` to create it)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="sync", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_pull = sub.add_parser("pull", help="pull state from OneDrive, acquire lock")
    p_pull.add_argument(
        "--force",
        action="store_true",
        help="take over a non-stale lock held by another machine",
    )
    sub.add_parser("push", help="push state back, release lock")
    sub.add_parser("status", help="show local vs remote state")
    args = ap.parse_args()
    if args.cmd == "pull":
        return cmd_pull(args)
    if args.cmd == "push":
        return cmd_push(args)
    if args.cmd == "status":
        return cmd_status(args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
