"""Migrate symbion.db from v13 schema to v14 (drop probe tables, keep core tables)."""
import sqlite3, sys, shutil
from pathlib import Path

KEPT_TABLES = [
    "messages", "summaries", "user_profile", "interactions", "human_feedback",
    "self_model", "tasks", "user_positions", "contradictions", "knowledge_gaps",
    "proactive_queue", "learning_metrics",
]

DROPPED_TABLES = [
    "eval_awareness_log", "sandbagging_log", "reward_hack_log", "adversarial_log",
    "snapshots", "behavioral_probes", "sycophancy_log", "deception_log",
    "sit_awareness_log", "frame_acceptance_log", "scheming_log",
    "swarm_runs", "swarm_agents", "resumable_tasks",
]

def migrate(src_path: str, dst_path: str):
    src = Path(src_path)
    dst = Path(dst_path)
    if not src.exists():
        print(f"Error: {src} does not exist"); sys.exit(1)
    if dst.exists():
        print(f"Error: {dst} already exists — move it first"); sys.exit(1)

    src_conn = sqlite3.connect(str(src))
    src_conn.row_factory = sqlite3.Row

    # Get all tables in source
    tables = [r[0] for r in src_conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'").fetchall()]

    # Create destination by copying schema + data for kept tables
    dst_conn = sqlite3.connect(str(dst))

    migrated = {}
    dropped = []

    for table in tables:
        if table in KEPT_TABLES:
            # Copy schema
            schema = src_conn.execute(
                "SELECT sql FROM sqlite_master WHERE type='table' AND name=?",
                (table,)).fetchone()[0]
            dst_conn.execute(schema)

            # Copy data
            rows = src_conn.execute(f"SELECT * FROM {table}").fetchall()
            if rows:
                cols = rows[0].keys()
                placeholders = ",".join("?" * len(cols))
                dst_conn.executemany(
                    f"INSERT INTO {table} ({','.join(cols)}) VALUES ({placeholders})",
                    [tuple(r) for r in rows])
            migrated[table] = len(rows)
        else:
            dropped.append(table)

    # Copy indexes for kept tables
    indexes = src_conn.execute(
        "SELECT sql FROM sqlite_master WHERE type='index' AND sql IS NOT NULL"
    ).fetchall()
    for idx in indexes:
        try:
            dst_conn.execute(idx[0])
        except sqlite3.OperationalError:
            pass  # index references dropped table

    dst_conn.commit()
    src_conn.close()
    dst_conn.close()

    print(f"\nMigration complete: {src} -> {dst}\n")
    print("Migrated tables:")
    for t, n in sorted(migrated.items()):
        print(f"  {t:<25} {n:>6} rows")
    print(f"\nDropped tables ({len(dropped)}):")
    for t in sorted(dropped):
        print(f"  {t}")
    print(f"\nTotal: {sum(migrated.values())} rows kept across {len(migrated)} tables")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: python {sys.argv[0]} <input.db> <output.db>")
        sys.exit(1)
    migrate(sys.argv[1], sys.argv[2])
