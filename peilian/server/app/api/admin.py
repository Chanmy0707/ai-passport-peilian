"""I5 场景/角色管理 API（后台前端 → 服务端，设计文档 §5.3）。

- POST/GET/PUT/DELETE /api/v1/scenarios[/{scenario_id}]  场景：名称/场景说明/绑定角色/对话时长/难度
- POST/GET/PUT/DELETE /api/v1/roles[/{role_id}]          角色：名称/性格描述/人设 prompt/TTS 音色

统一 {data, error} 包裹，认证 X-Admin-Token，错误码 401/404/422（§5.3）。
kb_id 为知识库预留字段，原样存取不校验（知识库后置）。
"""
import uuid

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from ..db import get_db, one, rows
from ..models import RoleCreate, RoleUpdate, ScenarioCreate, ScenarioUpdate, fail, ok
from .auth import check_admin

router = APIRouter()

_STATUS = {"UNAUTHORIZED": 401, "SCENARIO_NOT_FOUND": 404, "ROLE_NOT_FOUND": 404, "INVALID_FIELD": 422}

_SCENARIO_FIELDS = ("name", "description", "role_id", "kb_id", "duration_minutes", "difficulty")
_ROLE_FIELDS = ("name", "personality", "system_prompt", "tts_voice")


def _err(code: str, message: str) -> JSONResponse:
    return JSONResponse(status_code=_STATUS[code], content=fail(code, message))


def _auth(request: Request) -> JSONResponse | None:
    if code := check_admin(request):
        return _err("UNAUTHORIZED", "管理员令牌无效")
    return None


def _update(conn, table: str, row_id, body_fields: dict) -> None:
    sets = ", ".join(f"{k} = ?" for k in body_fields)
    conn.execute(f"UPDATE {table} SET {sets} WHERE id = ?", [*body_fields.values(), row_id])


# ---------- I5-1 场景 ----------

@router.get("/scenarios")
def list_scenarios(request: Request):
    if e := _auth(request):
        return e
    with get_db() as conn:
        items = rows(
            conn.execute(
                "SELECT s.id AS scenario_id, s.name, s.description, s.role_id, s.kb_id, "
                "s.duration_minutes, s.difficulty, r.name AS role_name "
                "FROM scenario s LEFT JOIN role_profile r ON r.id = s.role_id ORDER BY s.id"
            )
        )
    return ok(items)


@router.post("/scenarios")
def create_scenario(body: ScenarioCreate, request: Request):
    if e := _auth(request):
        return e
    if not body.name or not body.name.strip():
        return _err("INVALID_FIELD", "name 必填且不能为空")
    scenario_id = body.scenario_id or uuid.uuid4().hex
    with get_db() as conn:
        if body.role_id is not None:
            if not one(conn.execute("SELECT 1 AS ok FROM role_profile WHERE id = ?", (body.role_id,))):
                return _err("INVALID_FIELD", "role_id 对应的角色不存在")
        conn.execute(
            "INSERT INTO scenario (id, name, description, role_id, kb_id, duration_minutes, difficulty) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (
                scenario_id,
                body.name,
                body.description,
                body.role_id,
                body.kb_id,
                body.duration_minutes,
                body.difficulty,
            ),
        )
        item = one(conn.execute("SELECT * FROM scenario WHERE id = ?", (scenario_id,)))
    return ok(item)


@router.get("/scenarios/{scenario_id}")
def get_scenario(scenario_id: str, request: Request):
    if e := _auth(request):
        return e
    with get_db() as conn:
        item = one(conn.execute("SELECT * FROM scenario WHERE id = ?", (scenario_id,)))
    if not item:
        return _err("SCENARIO_NOT_FOUND", "场景不存在")
    return ok(item)


@router.put("/scenarios/{scenario_id}")
def update_scenario(scenario_id: str, body: ScenarioUpdate, request: Request):
    if e := _auth(request):
        return e
    fields = {k: v for k, v in body.model_dump(exclude_unset=True).items()}
    if "name" in fields and (not fields["name"] or not str(fields["name"]).strip()):
        return _err("INVALID_FIELD", "name 不能为空")
    with get_db() as conn:
        if not one(conn.execute("SELECT 1 AS ok FROM scenario WHERE id = ?", (scenario_id,))):
            return _err("SCENARIO_NOT_FOUND", "场景不存在")
        if fields.get("role_id") is not None and "role_id" in fields:
            if not one(conn.execute("SELECT 1 AS ok FROM role_profile WHERE id = ?", (fields["role_id"],))):
                return _err("INVALID_FIELD", "role_id 对应的角色不存在")
        if fields:
            _update(conn, "scenario", scenario_id, fields)
        item = one(conn.execute("SELECT * FROM scenario WHERE id = ?", (scenario_id,)))
    return ok(item)


@router.delete("/scenarios/{scenario_id}")
def delete_scenario(scenario_id: str, request: Request):
    if e := _auth(request):
        return e
    with get_db() as conn:
        if not one(conn.execute("SELECT 1 AS ok FROM scenario WHERE id = ?", (scenario_id,))):
            return _err("SCENARIO_NOT_FOUND", "场景不存在")
        conn.execute("DELETE FROM scenario WHERE id = ?", (scenario_id,))
    return ok({"scenario_id": scenario_id})


# ---------- I5-2 角色 ----------

@router.get("/roles")
def list_roles(request: Request):
    if e := _auth(request):
        return e
    with get_db() as conn:
        items = rows(conn.execute("SELECT * FROM role_profile ORDER BY id"))
    return ok(items)


@router.post("/roles")
def create_role(body: RoleCreate, request: Request):
    if e := _auth(request):
        return e
    if not body.name or not body.name.strip():
        return _err("INVALID_FIELD", "name 必填且不能为空")
    with get_db() as conn:
        cur = conn.execute(
            "INSERT INTO role_profile (name, personality, system_prompt, tts_voice) VALUES (?, ?, ?, ?)",
            (body.name, body.personality, body.system_prompt, body.tts_voice),
        )
        item = one(conn.execute("SELECT * FROM role_profile WHERE id = ?", (cur.lastrowid,)))
    return ok(item)


@router.get("/roles/{role_id}")
def get_role(role_id: int, request: Request):
    if e := _auth(request):
        return e
    with get_db() as conn:
        item = one(conn.execute("SELECT * FROM role_profile WHERE id = ?", (role_id,)))
    if not item:
        return _err("ROLE_NOT_FOUND", "角色不存在")
    return ok(item)


@router.put("/roles/{role_id}")
def update_role(role_id: int, body: RoleUpdate, request: Request):
    if e := _auth(request):
        return e
    fields = {k: v for k, v in body.model_dump(exclude_unset=True).items()}
    if "name" in fields and (not fields["name"] or not str(fields["name"]).strip()):
        return _err("INVALID_FIELD", "name 不能为空")
    with get_db() as conn:
        if not one(conn.execute("SELECT 1 AS ok FROM role_profile WHERE id = ?", (role_id,))):
            return _err("ROLE_NOT_FOUND", "角色不存在")
        if fields:
            _update(conn, "role_profile", role_id, fields)
        item = one(conn.execute("SELECT * FROM role_profile WHERE id = ?", (role_id,)))
    return ok(item)


@router.delete("/roles/{role_id}")
def delete_role(role_id: int, request: Request):
    if e := _auth(request):
        return e
    with get_db() as conn:
        if not one(conn.execute("SELECT 1 AS ok FROM role_profile WHERE id = ?", (role_id,))):
            return _err("ROLE_NOT_FOUND", "角色不存在")
        conn.execute("DELETE FROM role_profile WHERE id = ?", (role_id,))
    return ok({"role_id": role_id})
