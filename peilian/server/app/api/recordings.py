"""I6 录音查询 API（后台前端 → 服务端，设计文档 §5.3）。

- GET    /api/v1/recordings?device_id=&date=&keyword=&page=&size=  → {data: {items,total,page,size}, error}
- GET    /api/v1/recordings/{id}                                   → {data: {…, segments, transcript_status}, error}
- DELETE /api/v1/recordings/{id}                                   → 物理删除（§6.2 级联 + 清理音频目录）
- POST   /api/v1/recordings/batch-delete  {"ids": [...]}           → 批量物理删除（缺失 id 跳过）

transcript_status：pending（会话上传中/未切段）/ processing（转写中）/ done（完成，含部分分段 failed）/ failed（全部失败）。
409 表示转写未完成（§6.2 查询未生成状态返回 409，前端轮询）。
列表"上传时间"按 uploaded_at 显示（为空回落 ended_at/started_at，§6.2）。
"""
import shutil

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from ..db import get_db, one, rows
from ..models import fail, ok
from ..storage.audio import AudioStore
from .auth import check_admin

router = APIRouter()

_STATUS = {
    "UNAUTHORIZED": 401,
    "RECORDING_NOT_FOUND": 404,
    "TRANSCRIPT_PENDING": 409,
}


def _err(status: int, code: str, message: str) -> JSONResponse:
    return JSONResponse(status_code=status, content=fail(code, message))


def _transcript_status(session: dict, segments: list[dict]) -> str:
    if not segments:
        return "pending" if session["status"] == "uploading" else "processing"
    statuses = [s["status"] for s in segments]
    if any(s == "pending" for s in statuses):
        return "processing"
    if all(s == "failed" for s in statuses):
        return "failed"
    return "done"


@router.get("/recordings")
def list_recordings(
    request: Request,
    device_id: str | None = None,
    date: str | None = None,
    keyword: str | None = None,
    page: int = 1,
    size: int = 20,
):
    if code := check_admin(request):
        return _err(_STATUS[code], code, "管理员令牌无效")
    page = max(page, 1)
    size = min(max(size, 1), 100)

    where, params = [], []
    if device_id:
        where.append("s.device_id = ?")
        params.append(device_id)
    if date:
        where.append("COALESCE(s.uploaded_at, s.ended_at, s.started_at) LIKE ?")
        params.append(f"{date}%")
    if keyword:
        where.append(
            "EXISTS (SELECT 1 FROM audio_segment a JOIN transcript t ON t.segment_id = a.id "
            "WHERE a.session_id = s.id AND t.content LIKE ?)"
        )
        params.append(f"%{keyword}%")
    where_sql = (" WHERE " + " AND ".join(where)) if where else ""

    with get_db() as conn:
        total = one(
            conn.execute(f"SELECT COUNT(*) AS n FROM recording_session s{where_sql}", params)
        )["n"]
        sessions = rows(
            conn.execute(
                f"SELECT s.* FROM recording_session s{where_sql} "
                "ORDER BY COALESCE(s.uploaded_at, s.ended_at, s.started_at) DESC LIMIT ? OFFSET ?",
                [*params, size, (page - 1) * size],
            )
        )
        items = []
        for s in sessions:
            segs = rows(
                conn.execute(
                    "SELECT status FROM audio_segment WHERE session_id = ?", (s["id"],)
                )
            )
            items.append(
                {
                    "id": s["id"],
                    "device_id": s["device_id"],
                    "started_at": s["started_at"],
                    "ended_at": s["ended_at"],
                    "uploaded_at": s["uploaded_at"],
                    "status": s["status"],
                    "transcript_status": _transcript_status(s, segs),
                    "segment_count": len(segs),
                }
            )
    return ok({"items": items, "total": total, "page": page, "size": size})


@router.get("/recordings/{recording_id}")
def get_recording(recording_id: str, request: Request):
    if code := check_admin(request):
        return _err(_STATUS[code], code, "管理员令牌无效")
    with get_db() as conn:
        session = one(conn.execute("SELECT * FROM recording_session WHERE id = ?", (recording_id,)))
        if not session:
            return _err(404, "RECORDING_NOT_FOUND", "录音会话不存在")
        segments = rows(
            conn.execute(
                "SELECT a.id, a.seq_start, a.seq_end, a.file_url, a.duration_ms, a.status, "
                "t.content AS text "
                "FROM audio_segment a LEFT JOIN transcript t ON t.segment_id = a.id "
                "WHERE a.session_id = ? ORDER BY a.seq_start",
                (recording_id,),
            )
        )
    status = _transcript_status(session, segments)
    if status in ("pending", "processing"):
        # §6.2 转写未生成返回 409，前端轮询
        return _err(409, "TRANSCRIPT_PENDING", "转写未完成，请稍后轮询")
    return ok(
        {
            "id": session["id"],
            "device_id": session["device_id"],
            "started_at": session["started_at"],
            "ended_at": session["ended_at"],
            "uploaded_at": session["uploaded_at"],
            "status": session["status"],
            "transcript_status": status,
            "segments": segments,
        }
    )


def _delete_session(conn, recording_id: str) -> None:
    """级联物理删除（§6.2）：transcript → audio_segment → session + 清理音频目录。"""
    conn.execute(
        "DELETE FROM transcript WHERE segment_id IN "
        "(SELECT id FROM audio_segment WHERE session_id = ?)",
        (recording_id,),
    )
    conn.execute("DELETE FROM audio_segment WHERE session_id = ?", (recording_id,))
    conn.execute("DELETE FROM recording_session WHERE id = ?", (recording_id,))
    shutil.rmtree(AudioStore().base_dir / recording_id, ignore_errors=True)


@router.delete("/recordings/{recording_id}")
def delete_recording(recording_id: str, request: Request):
    """物理删除录音会话（§5.3 I6-4 / §6.2）。"""
    if code := check_admin(request):
        return _err(_STATUS[code], code, "管理员令牌无效")
    with get_db() as conn:
        session = one(
            conn.execute("SELECT id FROM recording_session WHERE id = ?", (recording_id,))
        )
        if not session:
            return _err(404, "RECORDING_NOT_FOUND", "录音会话不存在")
        _delete_session(conn, recording_id)
    return ok({"id": recording_id, "deleted": True})


class _BatchDeleteIn(BaseModel):
    ids: list[str]


@router.post("/recordings/batch-delete")
def batch_delete_recordings(body: _BatchDeleteIn, request: Request):
    """批量物理删除录音会话（§5.3 I6-5）：逐个级联删除，缺失的 id 跳过。"""
    if code := check_admin(request):
        return _err(_STATUS[code], code, "管理员令牌无效")
    deleted = []
    with get_db() as conn:
        for rid in body.ids:
            if one(conn.execute("SELECT id FROM recording_session WHERE id = ?", (rid,))):
                _delete_session(conn, rid)
                deleted.append(rid)
    return ok({"deleted": deleted, "count": len(deleted)})
