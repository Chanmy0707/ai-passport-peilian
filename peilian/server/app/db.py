"""SQLite 访问层（stdlib sqlite3，不用 ORM，见计划 Assumptions）。"""
import sqlite3
from contextlib import contextmanager
from pathlib import Path
from threading import Lock

from .config import SERVER_ROOT, settings

_write_lock = Lock()


@contextmanager
def get_db():
    conn = sqlite3.connect(settings.db_file(), check_same_thread=False)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_db() -> None:
    schema = (SERVER_ROOT / "schema.sql").read_text(encoding="utf-8")
    with _write_lock, get_db() as conn:
        conn.executescript(schema)


def rows(conn: sqlite3.Cursor) -> list[dict]:
    return [dict(r) for r in conn.fetchall()]


def one(conn: sqlite3.Cursor) -> dict | None:
    row = conn.fetchone()
    return dict(row) if row else None
