"""I3 工牌配置 + I4 场景列表（设备侧 HTTP，设计文档 §5.2）。

- GET  /api/v1/devices                      X-Admin-Token   → 设备列表（含联网时间与工牌配置状态）
- GET  /api/v1/devices/{device_id}/badge   X-Device-Token  → badge 对象（含 updated_at）
                                                           （管理员令牌亦可读取，用于后台回填现有配置）
- PUT  /api/v1/devices/{device_id}/badge   X-Admin-Token   → 更新工牌（后台配置）
- GET  /api/v1/devices/{device_id}/scenarios   X-Device-Token  → 场景列表（含场景说明与对话时长）

成功响应为裸对象（契约 §5.2），错误统一 {error: {code, message}}。
"""
import time

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from ..db import get_db, one, rows
from ..models import BadgeUpdate, ok
from .auth import check_admin, check_device

router = APIRouter()

_STATUS = {
    "UNAUTHORIZED": 401,
    "DEVICE_NOT_FOUND": 404,
    "INVALID_FIELD": 422,
    "RATE_LIMITED": 429,
}


def _err(code: str, message: str) -> JSONResponse:
    return JSONResponse(
        status_code=_STATUS.get(code, 400),
        content={"error": {"code": code, "message": message}},
    )


def _with_avatar_url(badge: dict) -> dict:
    """契约 §5.2：附 avatar_url（静态素材相对路径，设备端按需下载缓存）。

    avatar_id 为空时默认 😀（grinning_face），作为初始化表情头像。
    """
    avatar_id = badge.get("avatar_id") or "grinning_face"
    badge["avatar_id"] = avatar_id
    badge["avatar_url"] = f"/emojis/64px/{avatar_id}.png"
    return badge


@router.get("/devices")
def list_devices(request: Request):
    """管理端设备列表：device 表 + 已配工牌状态，供后台工牌弹窗设备选择（含联网时间）。"""
    if code := check_admin(request):
        return _err(code, "管理员令牌无效")
    with get_db() as conn:
        items = rows(
            conn.execute(
                "SELECT d.device_id, d.name, d.last_seen, d.fw_version, "
                "b.name AS badge_name, b.updated_at AS badge_updated_at "
                "FROM device d LEFT JOIN badge_profile b ON b.device_id = d.device_id "
                "ORDER BY d.device_id"
            )
        )
    return ok(items)


@router.get("/devices/{device_id}/badge")
def get_badge(device_id: str, request: Request):
    """设备令牌或管理员令牌均可读取：设备拉取显示，管理员用于后台回填现有配置。"""
    device_err = check_device(request)
    if device_err and (admin_err := check_admin(request)):
        return _err(device_err, "设备或管理员令牌无效")
    with get_db() as conn:
        badge = one(
            conn.execute(
                "SELECT name, title, employee_no, qr_text, avatar_id, updated_at "
                "FROM badge_profile WHERE device_id = ?",
                (device_id,),
            )
        )
    if not badge:
        return _err("DEVICE_NOT_FOUND", "该设备未配置工牌")
    return _with_avatar_url(badge)


@router.put("/devices/{device_id}/badge")
def update_badge(device_id: str, body: BadgeUpdate, request: Request):
    if code := check_admin(request):
        return _err(code, "管理员令牌无效")
    if not body.name or not body.name.strip():
        return _err("INVALID_FIELD", "name 必填且不能为空")
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    with get_db() as conn:
        conn.execute("INSERT OR IGNORE INTO device (device_id, name) VALUES (?, ?)", (device_id, body.name))
        conn.execute(
            "INSERT INTO badge_profile (device_id, name, title, employee_no, qr_text, avatar_id, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(device_id) DO UPDATE SET name=excluded.name, title=excluded.title, "
            "employee_no=excluded.employee_no, qr_text=excluded.qr_text, avatar_id=excluded.avatar_id, "
            "updated_at=excluded.updated_at",
            (device_id, body.name, body.title, body.employee_no, body.qr_text, body.avatar_id, now),
        )
        badge = one(
            conn.execute(
                "SELECT name, title, employee_no, qr_text, avatar_id, updated_at "
                "FROM badge_profile WHERE device_id = ?",
                (device_id,),
            )
        )
    return _with_avatar_url(badge)


@router.get("/devices/{device_id}/scenarios")
def list_scenarios(device_id: str, request: Request):
    if code := check_device(request):
        return _err(code, "设备令牌无效或触发限流")
    with get_db() as conn:
        exists = one(conn.execute("SELECT 1 AS ok FROM device WHERE device_id = ?", (device_id,)))
        if not exists:
            return _err("DEVICE_NOT_FOUND", "未知设备")
        items = rows(
            conn.execute(
                "SELECT id AS scenario_id, name, description, difficulty, duration_minutes "
                "FROM scenario ORDER BY id"
            )
        )
    return items
