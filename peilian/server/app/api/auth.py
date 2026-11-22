"""认证与限流（设计文档 §5.2/§5.3）。

- 管理端登录（MVP 单账号）：POST /api/v1/admin/login，用户名密码校验通过后下发管理员令牌
- 后台管理员会话（MVP 单账号）：请求头 X-Admin-Token
- 设备预共享 token：请求头 X-Device-Token（HTTP 侧；WS 侧在 hello 校验）
- 429 RATE_LIMITED：设备接口与登录接口按客户端 IP 滑动窗口限流
"""
import time
from collections import deque

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from ..config import settings
from ..models import AdminLogin, fail, ok

RATE_LIMIT_PER_MINUTE = 120
_hits: dict[str, deque] = {}

router = APIRouter()


def _rate_limited(client: str) -> bool:
    now = time.monotonic()
    q = _hits.setdefault(client, deque())
    while q and now - q[0] > 60.0:
        q.popleft()
    if len(q) >= RATE_LIMIT_PER_MINUTE:
        return True
    q.append(now)
    return False


def check_admin(request: Request) -> str | None:
    """返回错误码，None 表示通过。"""
    if request.headers.get("X-Admin-Token") != settings.admin_token:
        return "UNAUTHORIZED"
    return None


def check_device(request: Request) -> str | None:
    if request.headers.get("X-Device-Token") != settings.device_token:
        return "UNAUTHORIZED"
    client = request.client.host if request.client else "unknown"
    if _rate_limited(client):
        return "RATE_LIMITED"
    return None


@router.post("/admin/login")
def admin_login(body: AdminLogin, request: Request):
    """管理端登录（MVP 单账号）：校验用户名密码后下发管理员令牌。"""
    client = request.client.host if request.client else "unknown"
    if _rate_limited(client):
        return JSONResponse(status_code=429, content=fail("RATE_LIMITED", "请求过于频繁，请稍后再试"))
    if body.username != settings.admin_username or body.password != settings.admin_password:
        return JSONResponse(status_code=401, content=fail("UNAUTHORIZED", "用户名或密码错误"))
    return ok({"token": settings.admin_token})
